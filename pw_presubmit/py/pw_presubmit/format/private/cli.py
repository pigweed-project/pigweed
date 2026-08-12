# Copyright 2025 The Pigweed Authors
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
"""A CLI utility that checks and fixes formatting for source files."""

import argparse
import itertools
import logging
import os
from pathlib import Path
import sys
from typing import Collection, Pattern, Sequence

from pw_cli import log
from pw_cli.collect_files import (
    collect_files_in_current_repo,
    file_summary,
)
from pw_cli.plural import plural
from pw_cli.tool_runner import BasicSubprocessRunner
from pw_presubmit.format.core import FileFormatter
from pw_presubmit.format.private import cli_support

_LOG: logging.Logger = logging.getLogger(__name__)

# Only use for Git, we need to use the system git 100% of the time.
_git_runner = BasicSubprocessRunner()


# Eventually, this should be public. It's currently private to prevent too many
# external dependencies while it's under development.
class FormattingSuite:
    """A suite of code formatters that may be run on a set of files."""

    def __init__(
        self,
        formatters: Collection[FileFormatter],
        formatter_fix_command: str,
    ):
        self._formatters = formatters
        self._formatter_fix_command = formatter_fix_command

    def main(self) -> int:
        """Entry point for the formatter CLI."""
        # Set up logging.
        log.install(logging.INFO)

        if 'BUILD_WORKING_DIRECTORY' in os.environ:
            os.chdir(os.environ['BUILD_WORKING_DIRECTORY'])
        parser = argparse.ArgumentParser(
            prog='./pw format', description=__doc__
        )
        cli_support.add_arguments(parser, default_to_fix=True)
        args = parser.parse_args()
        return 0 if self.format_files(**vars(args)) else 1

    def format_files(
        self,
        paths: Collection[str | Path],
        base: str | None,
        exclude: Collection[Pattern] = tuple(),
        apply_fixes: bool = True,
        jobs: int | None = None,
        directory: Path | Sequence[Path] | None = None,
    ) -> bool:
        """Formats files in a repository.

        Args:
            paths: File paths and pathspects to format.
            base: Filters paths to only include files modified since this
                specified Git ref.
            exclude: Regex patterns to exclude from the set of collected files.
            apply_fixes: Whether or not to apply formatting fixes to files.
            jobs: Number of parallel jobs to use.
            directory: Directories to format files from.
        """
        dirs: list[Path] = []
        if directory:
            if isinstance(directory, (Path, str)):
                dirs = [Path(directory).resolve()]
            else:
                dirs = [Path(d).resolve() for d in directory]
        else:
            dirs = [Path.cwd()]

        all_files_list: list[Path] = []
        old_cwd = Path.cwd()
        try:
            for d in dirs:
                os.chdir(d)
                repo_files = collect_files_in_current_repo(
                    paths,
                    _git_runner,
                    modified_since_git_ref=base,
                    exclude_patterns=exclude,
                    action_flavor_text='Formatting',
                )
                all_files_list.extend(repo_files)
        finally:
            os.chdir(old_cwd)
        all_files = sorted(set(all_files_list))

        _LOG.info('Checking formatting for %s', plural(all_files, 'file'))

        for line in file_summary(
            cli_support.relativize_paths(all_files, Path.cwd())
        ):
            print(line, file=sys.stderr)

        all_files = list(cli_support.filter_exclusions(all_files))

        files_by_formatter = cli_support.map_files_to_formatters(
            all_files, self._formatters
        )

        findings = cli_support.check(files_by_formatter, jobs=jobs)
        findings_as_list = list(
            itertools.chain.from_iterable(findings.values())
        )

        cli_support.summarize_findings(
            findings_as_list,
            log_fix_command=(not apply_fixes),
            log_oneliner_summary=True,
            formatter_fix_command=self._formatter_fix_command,
        )

        if not findings:
            _LOG.info('Congratulations! No formatting changes needed')
            return True

        if not apply_fixes:
            _LOG.error('Formatting errors found')
            return False

        _LOG.info(
            'Applying formatting fixes to %s',
            plural(findings_as_list, 'file'),
        )
        fix_errors = cli_support.fix(findings)
        if fix_errors:
            _LOG.error('Failed to apply all formatting fixes')
            return False

        _LOG.info('Formatting fixes applied successfully')
        return True
