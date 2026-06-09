#!/usr/bin/env python3
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
"""Checks that the given compile_commands.json database is valid.

In general, "valid" means:
  - Follows the JSON schema for compile_commands.json
  - Contains compile commands for all source files for all deps of the
    specified target
  - Contains no duplicate file entries
  - Contains no unnecessary file entries (i.e. no entries for files that are
    not deps of the specified target)
"""

import argparse
from collections.abc import Iterator
import io
import json
import logging
from pathlib import Path
import sys

from jsonschema import validate  # type: ignore
from jsonschema.exceptions import ValidationError  # type: ignore

from pw_build import bazel_cquery
from pw_build.bazel_label import BazelLabel
from pw_cli import color

_LOG = logging.getLogger(__name__)

_C_SOURCE_FILE_EXTENSIONS = ('.c',)
_CPP_SOURCE_FILE_EXTENSIONS = (
    '.C',
    '.cc',
    '.cpp',
    '.CPP',
    '.c++',
    '.cp',
    '.cxx',
)
_HEADER_FILE_EXTENSIONS = ('.h', '.hpp', '.hh', '.hxx', '.h++')
_SOURCE_FILE_EXTENSIONS = (
    _C_SOURCE_FILE_EXTENSIONS + _CPP_SOURCE_FILE_EXTENSIONS
)
_INCLUDE_PREFIXES = ('-I', '-isystem')

_DB_JSON_SCHEMA_STRICT = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "file": {"type": "string"},
            "directory": {"type": "string"},
            "arguments": {"type": "array", "items": {"type": "string"}},
            "command": {"type": "string"},
            "output": {"type": "string"},
        },
        "additionalProperties": False,
        "oneOf": [
            {
                "required": ["file", "directory", "arguments"],
            },
            {
                "required": ["file", "directory", "command"],
            },
        ],
    },
}

_DB_JSON_SCHEMA_LAX = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "file": {"type": "string"},
            "directory": {"type": "string"},
            "arguments": {"type": "array", "items": {"type": "string"}},
            "command": {"type": "string"},
            "output": {"type": "string"},
        },
        "additionalProperties": True,
        "oneOf": [
            {
                "required": ["file", "directory", "arguments"],
            },
            {
                "required": ["file", "directory", "command"],
            },
        ],
    },
}


class Options:
    """Options for verify_db."""

    def __init__(self, args: argparse.Namespace | None = None):
        self.checks = {
            'format': True,
            'missing': True,
            'duplicate': True,
            'unnecessary': True,
            'virtual_include': True,
            'unexpected_source_file': True,
        }
        self.strict_format = False
        self.ignore_header_entries = False
        self.should_continue = False
        self.verbose = 0
        self.target = None
        self.platform = '@bazel_tools//tools:host_platform'
        self.db_file = None

        if args is not None:
            self._args_to_options(args)

    def __str__(self):
        return (
            f'Options(checks={self.checks}, strict_format={self.strict_format},'
            f' ignore_header_entries={self.ignore_header_entries},'
            f' should_continue={self.should_continue}, verbose={self.verbose},'
            f' target={self.target}, platform={self.platform},'
            f' db_file={self.db_file})'
        )

    def _args_to_options(self, args: argparse.Namespace):
        checks = [
            args.format_check,
            args.missing_check,
            args.duplicate_check,
            args.unnecessary_check,
            args.virtual_include_check,
            args.unexpected_source_file_check,
        ]
        # If none of the check flags were provided, enable all checks.
        if all(arg is None for arg in checks):
            args.all_checks = True

        if args.all_checks:
            self.checks['format'] = True
            self.checks['missing'] = True
            self.checks['duplicate'] = True
            self.checks['unnecessary'] = True
            self.checks['virtual_include'] = True
            self.checks['unexpected_source_file'] = True
        if args.format_check is not None:
            self.checks['format'] = args.format_check
        if args.missing_check is not None:
            self.checks['missing'] = args.missing_check
        if args.duplicate_check is not None:
            self.checks['duplicate'] = args.duplicate_check
        if args.unnecessary_check is not None:
            self.checks['unnecessary'] = args.unnecessary_check
        if args.virtual_include_check is not None:
            self.checks['virtual_include'] = args.virtual_include_check
        if args.unexpected_source_file_check is not None:
            self.checks['unexpected_source_file'] = (
                args.unexpected_source_file_check
            )
        if args.strict_format is not None:
            self.strict_format = args.strict_format
        if args.ignore_header_entries is not None:
            self.ignore_header_entries = args.ignore_header_entries
        if args.should_continue is not None:
            self.should_continue = args.should_continue
        if args.verbose is not None:
            self.verbose = args.verbose
        if args.target is not None:
            self.target = args.target
        else:
            # If no target is specified, we can't check for missing or
            # unnecessary entries, so we disable those checks.
            self.checks['missing'] = False
            self.checks['unnecessary'] = False
        if args.platform is not None:
            self.platform = args.platform
        if args.db_file is not None:
            self.db_file = args.db_file

    def all_checks(self, enable: bool):
        self.checks['format'] = enable
        self.checks['missing'] = enable
        self.checks['duplicate'] = enable
        self.checks['unnecessary'] = enable
        self.checks['virtual_include'] = enable
        self.checks['unexpected_source_file'] = enable

    def format_check(self, enable: bool, strict: bool = False):
        self.checks['format'] = enable
        self.strict_format = strict

    def missing_check(self, enable: bool):
        self.checks['missing'] = enable

    def duplicate_check(self, enable: bool):
        self.checks['duplicate'] = enable

    def unnecessary_check(self, enable: bool):
        self.checks['unnecessary'] = enable

    def virtual_include_check(self, enable: bool):
        self.checks['virtual_include'] = enable

    def unexpected_source_file_check(self, enable: bool):
        self.checks['unexpected_source_file'] = enable


def _parse_args(args: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    checks_group = parser.add_argument_group(
        'Available checks',
        'If no checks are specified all checks will run. If at least one check'
        ' is specified, only the specified checks will run.',
    )

    checks_group.add_argument(
        '--all-checks',
        action='store_true',
        help='Enable all checks. Subsequent --no-* flags will disable'
        ' individual checks.',
    )

    checks_group.add_argument(
        '--format-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for database format errors',
    )

    checks_group.add_argument(
        '--missing-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for missing entries in the database',
    )

    checks_group.add_argument(
        '--duplicate-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for duplicate entries in the database',
    )

    checks_group.add_argument(
        '--unnecessary-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for unnecessary entries in the database',
    )

    checks_group.add_argument(
        '--virtual-include-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for virtual include paths in the database',
    )

    checks_group.add_argument(
        '--unexpected-source-file-check',
        action=argparse.BooleanOptionalAction,
        default=None,
        help='Check for unexpected source files in the compilation commands',
    )

    parser.add_argument(
        '--strict-format',
        action='store_true',
        help='Use strict format checking (implies --format-check)',
    )
    parser.add_argument(
        '--ignore-header-entries',
        action='store_true',
        help='Ignore header entries in the unexpected source file and'
        ' unnecessary checks',
    )
    parser.add_argument(
        '--continue',
        dest="should_continue",
        action='store_true',
        help='Continue checking even after finding an error',
    )
    parser.add_argument(
        '-v',
        '--verbose',
        action='count',
        default=0,
        help='Enable verbose logging',
    )

    target_group = parser.add_mutually_exclusive_group(required=True)
    target_group.add_argument(
        '--target', type=str, help='The bazel target to check against'
    )
    target_group.add_argument(
        '--no-target',
        action='store_true',
        help='Don\'t check against a specific target (implies'
        ' --no-missing-check, --no-unnecessary-check)',
    )

    parser.add_argument(
        '--platform',
        type=str,
        default='@bazel_tools//tools:host_platform',
        help='The bazel platform',
    )

    parser.add_argument(
        'db_file', type=str, help='Path to compile_commands.json'
    )

    if args is not None:
        return parser.parse_args(args)
    return parser.parse_args()


class PrettyFormatter(logging.Formatter):
    """A logging formatter that tunes logging for this script."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._colors = color.colors()
        self._color_enabled = color.is_enabled()

    def _gray(self, msg: str) -> str:
        if self._color_enabled:
            return self._colors.gray(msg)
        return msg

    def format(self, record: logging.LogRecord) -> str:
        """Formats the log record."""
        message = record.getMessage()
        if record.levelno >= logging.ERROR:
            level_prefix = '❌ '
            message = self._colors.red(message)
        elif record.levelno >= logging.WARNING:
            level_prefix = '⚠️ '
            message = self._colors.yellow(message)
        elif record.levelno == logging.DEBUG or record.levelno == logging.INFO:
            level_prefix = ''
            message = self._gray(message)
        else:
            level_prefix = ''
        return f'{level_prefix}{message}'


def _setup_logging(log_level: int):
    handler = logging.StreamHandler()
    handler.setFormatter(PrettyFormatter())
    root_logger = logging.getLogger()
    root_logger.addHandler(handler)
    root_logger.setLevel(log_level)


def _get_target_source_files(target: BazelLabel, platform: str) -> list[Path]:
    """Get the source files for a bazel target."""
    srcs = bazel_cquery.source_files(target, platform=platform)
    gens = bazel_cquery.generated_files(target, platform=platform)

    source_files = []
    for file in srcs + gens:
        if file.suffix in _SOURCE_FILE_EXTENSIONS:
            source_files.append(file)

    return source_files


def _load_db(db_file: io.IOBase) -> list[dict] | None:
    """Load the database from a file."""
    try:
        return json.load(db_file)
    except json.JSONDecodeError as e:
        _LOG.error('Failed to read database: %s', e)
        return None


def _get_db_source_files(db: list[dict]) -> list[Path]:
    """Get the source files from a compile_commands.json file."""
    return [Path(entry["file"]) for entry in db]


def _convert_command_to_arguments(command: str) -> list[str]:
    """Convert a command string to a list of arguments."""
    return command.split(' ')


def _find_missing(
    target_files: list[Path], db_files: list[Path]
) -> Iterator[Path]:
    """Find missing entries between target files and database files."""
    for src in target_files:
        if src not in db_files:
            yield src
        else:
            _LOG.debug('Found source file in db: %s', src)


def _find_duplicates(db_files: list[Path]) -> Iterator[Path]:
    """Find duplicate entries in database files."""
    duplicates = []
    for file in db_files:
        if db_files.count(file) > 1 and file not in duplicates:
            duplicates.append(file)
            yield file


def _find_unnecessary(
    target_files: list[Path], db_files: list[Path]
) -> Iterator[Path]:
    """Find unnecessary entries in database files."""
    for file in db_files:
        if file not in target_files:
            yield file


def format_check(db: list[dict], strict: bool = False) -> bool:
    """Check that the database adheres to the expected schema.

    Args:
        db: The database object.
        strict: Whether to use strict format checking.

    Returns:
        True if the database adheres to the expected schema, False otherwise.
    """
    try:
        if strict:
            validate(instance=db, schema=_DB_JSON_SCHEMA_STRICT)
        else:
            validate(instance=db, schema=_DB_JSON_SCHEMA_LAX)
    except ValidationError as e:
        for path in e.path:
            if isinstance(path, int):
                _LOG.warning(
                    'Found format error in database on file entry: %s',
                    db[path],
                )
            else:
                _LOG.warning(
                    'Found format error in database on entry: %s', path
                )
            _LOG.debug('Format error: %s', e)
        return False

    _LOG.info('Database is valid.')
    return True


def missing_check(
    target_files: list[Path],
    db_files: list[Path],
    should_continue: bool = False,
) -> bool:
    """Check for missing entries in database files.

    Args:
        target_files: List of source files for the target and all deps.
        db_files: List of source files in the database.
        should_continue: Whether to continue checking after finding an error.

    Returns:
        True if no missing entries are found, False otherwise.
    """
    result = True
    missing = _find_missing(target_files, db_files)
    for file in missing:
        result = False
        _LOG.warning(
            'Source file %s not found in database but required by target.', file
        )
        if not should_continue:
            return result

    if result:
        _LOG.info('All target source files found in database.')

    return result


def duplicate_check(
    db_files: list[Path],
    ignore_header_entries: bool = False,
    should_continue: bool = False,
) -> bool:
    """Check for duplicate entries in database files.

    Args:
        db_files: List of source files in the database.
        ignore_header_entries: Whether to ignore header entries.
        should_continue: Whether to continue checking after finding an error.

    Returns:
        True if no duplicate entries are found, False otherwise.
    """
    result = True
    duplicates = _find_duplicates(db_files)
    for dup in duplicates:
        # TODO: https://pwbug.dev/500484180 - Remove after fix is landed
        if ignore_header_entries and dup.suffix in _HEADER_FILE_EXTENSIONS:
            continue
        result = False
        _LOG.warning('Found duplicate entries for file: %s', dup)
        if not should_continue:
            return result

    if result:
        _LOG.info('No duplicate entries found in database.')

    return result


def unnecessary_check(
    target_files: list[Path],
    db_files: list[Path],
    ignore_header_entries: bool = False,
    should_continue: bool = False,
) -> bool:
    """Check for unnecessary entries in database files.

    Args:
        target_files: List of source files for the target.
        db_files: List of source files in the database.
        ignore_header_entries: Whether to ignore header entries.
        should_continue: Whether to continue checking after finding an error.

    Returns:
        True if no unnecessary entries are found, False otherwise.
    """
    if ignore_header_entries:
        db_files = [
            f for f in db_files if f.suffix not in _HEADER_FILE_EXTENSIONS
        ]
    result = True
    unnecessary = _find_unnecessary(target_files, db_files)
    for file in unnecessary:
        result = False
        _LOG.warning('Found unnecessary entry: %s', file)
        if not should_continue:
            return result

    if result:
        _LOG.info('No unnecessary entries found in database.')

    return result


def virtual_include_check(
    db: list[dict],
    should_continue: bool = False,
) -> bool:
    """Check for virtual include paths in database files.

    Args:
        db: The database object.
        should_continue: Whether to continue checking after finding an error.

    Returns:
        True if no virtual include paths are found, False otherwise.
    """
    result = True
    for entry in db:
        _LOG.debug('Checking file: %s', entry['file'])
        if 'command' in entry:
            args = _convert_command_to_arguments(entry['command'])
        elif 'arguments' in entry:
            args = entry['arguments']
        else:
            _LOG.error(
                'Entry %s has no arguments or command field.', entry['file']
            )
            result = False
            return result

        entry_dir = Path(entry.get('directory', '.'))
        for arg in args:
            _LOG.debug('Checking arg: %s', arg)
            if '/_virtual_includes/' in arg:
                prefix = ""
                for p in _INCLUDE_PREFIXES:
                    if arg.startswith(p):
                        prefix = p
                        break
                path_part = arg[len(prefix) :]
                full_path = entry_dir / path_part
                # Unused implicit toolchain/platform dependencies (like
                # pw_string) may inject virtual includes that are not compiled
                # (hence they don't exist on disk) and cannot be resolved via
                # aspect. Since they are unused and clangd does not need them,
                # we can safely skip them.
                if full_path.exists():
                    result = False
                    _LOG.warning('Found virtual include path: %s', arg)
                    if not should_continue:
                        return result

    if result:
        _LOG.info('No virtual include paths found in database.')

    return result


def unexpected_source_files_check(
    db: list[dict],
    ignore_header_entries: bool = False,
    should_continue: bool = False,
) -> bool:
    """Check for unexpected source files in database files.

    Args:
        db: The database object.
        ignore_header_entries: Whether to ignore header entries. Header entries
            will always have a .cc file in the command (the actual file being
            compiled).
        should_continue: Whether to continue checking after finding an error.

    Returns:
        True if no unexpected source files are found, False otherwise.
    """
    result = True
    for entry in db:
        if ignore_header_entries and entry['file'].endswith(
            _HEADER_FILE_EXTENSIONS
        ):
            continue
        _LOG.debug('Checking file: %s', entry['file'])
        if 'command' in entry:
            args = _convert_command_to_arguments(entry['command'])
        elif 'arguments' in entry:
            args = entry['arguments']
        else:
            _LOG.error(
                'Entry %s has no arguments or command field.', entry['file']
            )
            result = False
            return result

        for arg in args:
            _LOG.debug('Checking arg: %s', arg)
            # Skip flags (which may contain other source files),
            # Skip the entry file itself,
            # Any other files that end with a source file extension are
            # unexpected
            if (
                not arg.startswith('-')
                and arg != entry['file']
                and arg.endswith(_SOURCE_FILE_EXTENSIONS)
            ):
                result = False
                _LOG.warning('Found unexpected source file in arg: %s', arg)
                if not should_continue:
                    return result

    if result:
        _LOG.info('No unexpected source files found in arguments.')

    return result


def verify_db(
    db: list[dict],
    target_files: list[Path] | None = None,
    options: Options = Options(),
) -> bool:
    """
    Verify the database is valid.

    Args:
        db: The database object.
        target_files: List of source files expected by the target.
        options: Options for the verification.

    Returns:
        True if the database is valid (selected checks pass), False otherwise.
    """

    if target_files is None:
        target_files = []
        _LOG.debug('No target files provided.')

    if not any(options.checks.values()):
        _LOG.warning('No checks enabled!')
        return True

    _LOG.debug('Verifying DB with options: %s', options)

    is_valid = True
    if options.checks['format']:
        if not format_check(db, options.strict_format):
            is_valid = False
            _LOG.warning('Database file is not formatted correctly.')
            if not options.should_continue:
                return is_valid

    db_files = _get_db_source_files(db)

    if options.checks['missing']:
        if not missing_check(target_files, db_files, options.should_continue):
            is_valid = False
            _LOG.warning('Missing entries in database.')
            if not options.should_continue:
                return is_valid
        else:
            _LOG.info('No missing entries found in database.')

    if options.checks['duplicate']:
        if not duplicate_check(
            db_files, options.ignore_header_entries, options.should_continue
        ):
            is_valid = False
            _LOG.warning('Duplicate entries found in database.')
            if not options.should_continue:
                return is_valid
        else:
            _LOG.info('No duplicate entries found in database.')

    if options.checks['unnecessary']:
        if not unnecessary_check(
            target_files,
            db_files,
            options.ignore_header_entries,
            options.should_continue,
        ):
            is_valid = False
            _LOG.warning('Unnecessary entries found in database.')
            if not options.should_continue:
                return is_valid
        else:
            _LOG.info('No unnecessary entries found in database.')

    if options.checks['virtual_include']:
        if not virtual_include_check(db, options.should_continue):
            is_valid = False
            _LOG.warning('Virtual include paths found in database.')
            if not options.should_continue:
                return is_valid
        else:
            _LOG.info('No virtual include paths found in database.')

    if options.checks['unexpected_source_file']:
        if not unexpected_source_files_check(
            db,
            options.ignore_header_entries,
            options.should_continue,
        ):
            is_valid = False
            _LOG.warning('Unexpected source files found in database.')
            if not options.should_continue:
                return is_valid
        else:
            _LOG.info('No unexpected source files found in database.')

    return is_valid


def main() -> int:
    """Main entry point for the verify_db tool."""
    args = _parse_args()

    if args.verbose == 0:
        log_level = logging.WARNING
    elif args.verbose == 1:
        log_level = logging.INFO
    else:
        log_level = logging.DEBUG

    _setup_logging(log_level)

    if args.target:
        _LOG.info('Getting source files for target %s...', args.target)
        target_srcs = _get_target_source_files(
            BazelLabel.from_string(args.target),
            args.platform,
        )
    else:
        target_srcs = []

    try:
        with open(args.db_file, 'r') as f:
            db = _load_db(f)
    except FileNotFoundError:
        _LOG.error('Database file not found: %s', args.db_file)
        return 1

    if db is None:
        return 1

    if not verify_db(db, target_srcs, Options(args)):
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
