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
"""Supporting helpers for format CLI tooling."""

import argparse
import collections
from concurrent.futures import ThreadPoolExecutor, Future
import json
import logging
from pathlib import Path
import re
import sys
import textwrap
from typing import (
    Collection,
    Dict,
    Iterable,
    Iterator,
    List,
    Mapping,
    Sequence,
    TextIO,
)

from pw_cli import argument_types, color
from pw_cli.collect_files import add_file_collection_arguments
from pw_cli.diff import colorize_diff
from pw_cli.plural import plural
from pw_config_loader import find_config
from pw_presubmit.format.core import (
    FileFormatter,
    FormatFixStatus,
    FormattedDiff,
)

_LOG: logging.Logger = logging.getLogger(__name__)


def findings_to_formatted_diffs(
    diffs: dict[Path, str]
) -> Sequence[FormattedDiff]:
    """Converts legacy formatter findings to structured findings."""
    return [
        FormattedDiff(
            ok=True,
            diff=finding,
            error_message=None,
            file_path=path,
        )
        for path, finding in diffs.items()
    ]


def filter_exclusions(file_paths: Iterable[Path]) -> Iterator[Path]:
    """Filters paths if they match an exclusion pattern in a pigweed.json."""
    # TODO: b/399204950 - Dedupe this with the FormatOptions class.
    paths_by_config = find_config.paths_by_nearest_config(
        "pigweed.json",
        file_paths,
    )
    for config, paths in paths_by_config.items():
        if config is None:
            yield from paths
            continue
        config_obj = json.loads(config.read_text())
        fmt = config_obj.get('pw', {}).get('pw_presubmit', {}).get('format', {})
        exclude = tuple(re.compile(x) for x in fmt.get('exclude', ()))
        relpaths = [(x.resolve().relative_to(config.parent), x) for x in paths]
        for relative_path, original_path in relpaths:
            # Yield the original path if none of the exclusion patterns match.
            if not [
                filt for filt in exclude if filt.search(str(relative_path))
            ]:
                yield original_path


def summarize_findings(
    findings: Sequence[FormattedDiff],
    log_fix_command: bool,
    log_oneliner_summary: bool,
    file: TextIO = sys.stdout,
    formatter_fix_command: str = 'pw format --fix',
) -> None:
    """Prints a summary of a format check's findings."""
    if not findings:
        return

    if log_oneliner_summary:
        _LOG.warning(
            'Found %s with formatting issues:',
            plural(findings, 'file'),
        )

    paths_to_fix = []
    for formatting_finding in findings:
        if not formatting_finding.ok:
            file.write(
                f'ERROR: Failed to format {formatting_finding.file_path}:\n'
            )
            if formatting_finding.error_message:
                file.write(
                    textwrap.indent(
                        formatting_finding.error_message,
                        '    ',
                    )
                )
        if not formatting_finding.diff:
            continue

        paths_to_fix.append(formatting_finding.file_path)
        diff = (
            colorize_diff(formatting_finding.diff)
            if color.is_enabled(file)
            else formatting_finding.diff
        )
        file.write(diff)

    if log_fix_command:

        def path_relative_to_cwd(path: Path):
            try:
                return Path(path).resolve().relative_to(Path.cwd().resolve())
            except ValueError:
                return Path(path).resolve()

        paths = " ".join([str(path_relative_to_cwd(p)) for p in paths_to_fix])
        message = f'  {formatter_fix_command} {paths}'
        _LOG.warning('To fix formatting, run:\n\n%s\n', message)


def add_arguments(
    parser: argparse.ArgumentParser, *, default_to_fix: bool
) -> None:
    """Adds formatting CLI arguments to an argument parser."""
    add_file_collection_arguments(parser)
    fix_mode = parser.add_mutually_exclusive_group()
    fix_mode.add_argument(
        '--check',
        action='store_false',
        dest='apply_fixes',
        help='{}Only display findings, do not apply formatting fixes.'.format(
            '(Default) ' if not default_to_fix else ''
        ),
    )
    fix_mode.add_argument(
        '--fix',
        action='store_true',
        dest='apply_fixes',
        help='{}Apply formatting fixes in place.'.format(
            '(Default) ' if default_to_fix else ''
        ),
    )
    parser.add_argument(
        '-j',
        '--jobs',
        type=int,
        help='Number of parallel jobs to use. Defaults to the number of CPUs.',
    )
    parser.add_argument(
        '-C',
        '--directory',
        type=argument_types.directory,
        action='append',
        default=[],
        help=argparse.SUPPRESS,
    )
    parser.set_defaults(apply_fixes=default_to_fix)


def relativize_paths(
    paths: Iterable[Path], relative_to: Path
) -> Iterable[Path]:
    """Relativizes paths when possible."""
    for path in paths:
        try:
            yield path.resolve().relative_to(relative_to.resolve())
        except ValueError:
            yield path


def check(
    files_by_formatter: Mapping[FileFormatter, Iterable[Path]],
    jobs: int | None = None,
) -> Mapping[FileFormatter, Sequence[FormattedDiff]]:
    """Returns expected diffs for files with incorrect formatting."""
    findings_by_formatter: Dict[FileFormatter, List[FormattedDiff]] = {}

    def check_file(
        code_formatter: FileFormatter, file_path: Path
    ) -> FormattedDiff | None:
        return code_formatter.get_formatting_diff(file_path)

    with ThreadPoolExecutor(max_workers=jobs) as exe:
        futures: Dict[
            Future[FormattedDiff | None],
            FileFormatter,
        ] = {}

        for code_formatter, files in files_by_formatter.items():
            _LOG.debug('Checking %s', ', '.join(str(f) for f in files))
            for file_path in files:
                future = exe.submit(check_file, code_formatter, file_path)
                futures[future] = code_formatter

        for future, code_formatter in futures.items():
            diff = future.result()
            if diff:
                if code_formatter not in findings_by_formatter:
                    findings_by_formatter[code_formatter] = []
                findings_by_formatter[code_formatter].append(diff)

    return findings_by_formatter


def fix(
    findings_by_formatter: Mapping[FileFormatter, Iterable[FormattedDiff]]
) -> Mapping[Path, FormatFixStatus]:
    """Fixes formatting errors in-place."""
    errors: Dict[Path, FormatFixStatus] = {}
    for formatter, diffs in findings_by_formatter.items():
        files_to_format = [diff.file_path for diff in diffs]
        statuses = formatter.format_files(files_to_format)
        successfully_formatted = set(files_to_format)
        for path, status in statuses:
            if not status.ok:
                successfully_formatted.remove(path)
                _LOG.error('Failed to format %s', path)
                errors[path] = status
            if status.error_message is not None:
                for line in status.error_message.splitlines():
                    _LOG.error('%s', line)
        if successfully_formatted:
            _LOG.info(
                'Successfully formatted %s',
                plural(successfully_formatted, formatter.mnemonic + ' file'),
            )

    return errors


def map_files_to_formatters(
    paths: Iterable[Path],
    formatters: Collection[FileFormatter],
) -> Mapping[FileFormatter, Iterable[Path]]:
    """Maps files to formatters."""
    formatting_map: Dict[FileFormatter, List[Path]] = collections.defaultdict(
        list
    )
    for path in paths:
        formatter_found = False
        for formatter in formatters:
            if formatter.file_patterns.matches(path):
                _LOG.debug('Formatting %s as %s', path, formatter.mnemonic)
                formatting_map[formatter].append(path)
                formatter_found = True
        if not formatter_found:
            _LOG.debug('No formatter found for %s', path)
    return formatting_map
