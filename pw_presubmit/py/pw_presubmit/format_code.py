#!/usr/bin/env python3

# Copyright 2020 The Pigweed Authors
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
"""Checks and fixes formatting for source files.

This uses clang-format, gn format, gofmt, and python -m yapf to format source
code. These tools must be available on the path when this script is invoked!
"""

import argparse
import collections
import difflib
import io
import logging
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import (
    Callable,
    Collection,
    Iterable,
    NamedTuple,
    Pattern,
)

from pw_cli.collect_files import (
    collect_files_in_current_repo,
    file_summary,
)
import pw_cli.color
import pw_cli.env
from pw_cli.file_filter import FileFilter
from pw_cli.plural import plural
import pw_env_setup.config_file
from pw_presubmit.presubmit import filter_paths
from pw_presubmit.presubmit_context import (
    FormatContext,
    FormatOptions,
)
from pw_presubmit import (
    PresubmitContext,
    PresubmitFailure,
)
from pw_presubmit import (
    git_repo,
    presubmit_context,
)
from pw_presubmit.format.bazel import (
    BuildifierFormatter,
    DEFAULT_BAZEL_FILE_PATTERNS,
)
from pw_presubmit.format.cmake import DEFAULT_CMAKE_FILE_PATTERNS
from pw_presubmit.format.core import FormattedDiff, FormatFixStatus
from pw_presubmit.format.css import DEFAULT_CSS_FILE_PATTERNS
from pw_presubmit.format import cpp
from pw_presubmit.format.cpp import ClangFormatFormatter
from pw_presubmit.format.gn import GnFormatter, DEFAULT_GN_FILE_PATTERNS
from pw_presubmit.format.go import GofmtFormatter, DEFAULT_GO_FILE_PATTERNS
from pw_presubmit.format.java import DEFAULT_JAVA_FILE_PATTERNS
from pw_presubmit.format.javascript import DEFAULT_JAVASCRIPT_FILE_PATTERNS
from pw_presubmit.format.json import (
    JsonFormatter,
    DEFAULT_JSON_FILE_PATTERNS,
)
from pw_presubmit.format.markdown import DEFAULT_MARKDOWN_FILE_PATTERNS
from pw_presubmit.format.owners import (
    OwnersFormatter,
    DEFAULT_OWNERS_FILE_PATTERNS,
)
from pw_presubmit.format.prettier import PrettierFormatter
from pw_presubmit.format.private.cli_support import (
    add_arguments,
    summarize_findings,
    findings_to_formatted_diffs,
)
from pw_presubmit.format.protobuf import DEFAULT_PROTOBUF_FILE_PATTERNS
from pw_presubmit.format.python import (
    BlackFormatter,
    DEFAULT_PYTHON_FILE_PATTERNS,
)
from pw_presubmit.format.rst import (
    RstFormatter,
    DEFAULT_RST_FILE_PATTERNS,
)
from pw_presubmit.format.starlark import DEFAULT_STARLARK_FILE_PATTERNS
from pw_presubmit.format.typescript import DEFAULT_TYPESCRIPT_FILE_PATTERNS
from pw_presubmit.format.whitespace import TrailingSpaceFormatter
from pw_presubmit.format.rust import (
    RustfmtFormatter,
    DEFAULT_RUST_FILE_PATTERNS,
)
from pw_presubmit.tools import (
    log_run,
    PresubmitToolRunner,
)

_LOG: logging.Logger = logging.getLogger(__name__)
_COLOR = pw_cli.color.colors()
_DEFAULT_PATH = Path('out', 'format')

_Context = PresubmitContext | FormatContext


def _ensure_newline(orig: bytes) -> bytes:
    if orig.endswith(b'\n'):
        return orig
    return orig + b'\nNo newline at end of file\n'


def _diff(path, original: bytes, formatted: bytes) -> str:
    original = _ensure_newline(original)
    formatted = _ensure_newline(formatted)
    return ''.join(
        difflib.unified_diff(
            original.decode(errors='replace').splitlines(True),
            formatted.decode(errors='replace').splitlines(True),
            f'{path}  (original)',
            f'{path}  (reformatted)',
        )
    )


FormatterT = Callable[[str, bytes], bytes]


def _diff_formatted(
    path, formatter: FormatterT, dry_run: bool = False
) -> str | None:
    """Returns a diff comparing a file to its formatted version."""
    with open(path, 'rb') as fd:
        original = fd.read()

    formatted = formatter(path, original)

    if dry_run:
        return None

    return None if formatted == original else _diff(path, original, formatted)


def _check_files(
    files, formatter: FormatterT, dry_run: bool = False
) -> dict[Path, str]:
    errors = {}

    for path in files:
        difference = _diff_formatted(path, formatter, dry_run)
        if difference:
            errors[path] = difference

    return errors


def _make_formatting_diff_dict(
    diffs: Iterable[FormattedDiff],
) -> dict[Path, str]:
    """Adapts the formatting check API to work with this presubmit tooling."""
    return {
        result.file_path: (
            result.diff if result.ok else str(result.error_message)
        )
        for result in diffs
    }


def _make_format_fix_error_output_dict(
    statuses: Iterable[tuple[Path, FormatFixStatus]],
) -> dict[Path, str]:
    """Adapts the formatter API to work with this presubmit tooling."""
    return {
        file_path: str(status.error_message) for file_path, status in statuses
    }


def clang_format_check(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = ClangFormatFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(ctx.paths, ctx.dry_run)
    )


def clang_format_fix(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = ClangFormatFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def typescript_format_check(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = PrettierFormatter(
        tool_runner=PresubmitToolRunner(),
    )
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(ctx.paths, ctx.dry_run)
    )


def typescript_format_fix(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = PrettierFormatter(
        tool_runner=PresubmitToolRunner(),
    )
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_gn_format(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = GnFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_gn_format(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = GnFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_bazel_format(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = BuildifierFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_bazel_format(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = BuildifierFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_owners_format(ctx: _Context) -> dict[Path, str]:
    formatter = OwnersFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_owners_format(ctx: _Context) -> dict[Path, str]:
    formatter = OwnersFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_go_format(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = GofmtFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_go_format(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = GofmtFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


# TODO: b/259595799 - Remove yapf support.
def _yapf(*args, **kwargs) -> subprocess.CompletedProcess:
    return log_run(
        ['python', '-m', 'yapf', '--parallel', *args],
        capture_output=True,
        **kwargs,
    )


_DIFF_START = re.compile(r'^--- (.*)\s+\(original\)$', flags=re.MULTILINE)


def check_py_format_yapf(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    process = _yapf('--diff', *ctx.paths)

    errors: dict[Path, str] = {}

    if process.stdout:
        raw_diff = process.stdout.decode(errors='replace')

        matches = tuple(_DIFF_START.finditer(raw_diff))
        for start, end in zip(matches, (*matches[1:], None)):
            errors[Path(start.group(1))] = raw_diff[
                start.start() : end.start() if end else None
            ]

    if process.stderr:
        _LOG.error(
            'yapf encountered an error:\n%s',
            process.stderr.decode(errors='replace').rstrip(),
        )
        errors.update({file: '' for file in ctx.paths if file not in errors})

    return errors


def fix_py_format_yapf(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    print_format_fix(_yapf('--in-place', *ctx.paths, check=True).stdout)
    return {}


def _enumerate_black_configs() -> Iterable[Path]:
    config = pw_env_setup.config_file.load()
    black_config_file = (
        config.get('pw', {})
        .get('pw_presubmit', {})
        .get('format', {})
        .get('black_config_file', {})
    )
    if black_config_file:
        explicit_path = Path(black_config_file)
        if not explicit_path.is_file():
            raise ValueError(f'Black config file not found: {explicit_path}')
        yield explicit_path
        return  # If an explicit path is provided, don't try implicit paths.

    if directory := os.environ.get('PW_PROJECT_ROOT'):
        yield Path(directory, '.black.toml')
        yield Path(directory, 'pyproject.toml')

    if directory := os.environ.get('PW_ROOT'):
        yield Path(directory, '.black.toml')
        yield Path(directory, 'pyproject.toml')


def _select_black_config_file() -> Path | bool:
    for config_location in _enumerate_black_configs():
        if config_location.is_file():
            return config_location
    return False


def check_py_format_black(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = BlackFormatter(
        _select_black_config_file(), tool_runner=PresubmitToolRunner()
    )
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_py_format_black(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = BlackFormatter(
        _select_black_config_file(), tool_runner=PresubmitToolRunner()
    )
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_py_format(ctx: _Context) -> dict[Path, str]:
    if ctx.format_options.python_formatter == 'black':
        return check_py_format_black(ctx)
    if ctx.format_options.python_formatter == 'yapf':
        return check_py_format_yapf(ctx)
    raise ValueError(ctx.format_options.python_formatter)


def fix_py_format(ctx: _Context) -> dict[Path, str]:
    if ctx.format_options.python_formatter == 'black':
        return fix_py_format_black(ctx)
    if ctx.format_options.python_formatter == 'yapf':
        return fix_py_format_yapf(ctx)
    raise ValueError(ctx.format_options.python_formatter)


def check_json_format(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = JsonFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def fix_json_format(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = JsonFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def check_trailing_space(ctx: _Context) -> dict[Path, str]:
    formatter = TrailingSpaceFormatter(
        file_patterns=FileFilter(), tool_runner=PresubmitToolRunner()
    )
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(ctx.paths, ctx.dry_run)
    )


def fix_trailing_space(ctx: _Context) -> dict[Path, str]:
    formatter = TrailingSpaceFormatter(
        file_patterns=FileFilter(), tool_runner=PresubmitToolRunner()
    )
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def rst_format_check(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = RstFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(
            ctx.paths,
            ctx.dry_run,
        )
    )


def rst_format_fix(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = RstFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def rust_format_check(ctx: _Context) -> dict[Path, str]:
    """Checks formatting; returns {path: diff} for files with bad formatting."""
    formatter = RustfmtFormatter(tool_runner=PresubmitToolRunner())
    return _make_formatting_diff_dict(
        formatter.get_formatting_diffs(ctx.paths, ctx.dry_run)
    )


def rust_format_fix(ctx: _Context) -> dict[Path, str]:
    """Fixes formatting for the provided files in place."""
    formatter = RustfmtFormatter(tool_runner=PresubmitToolRunner())
    return _make_format_fix_error_output_dict(formatter.format_files(ctx.paths))


def print_format_fix(stdout: bytes):
    """Prints the output of a format --fix call."""
    for line in stdout.splitlines():
        _LOG.info('Fix cmd stdout: %r', line.decode('utf-8'))


class CodeFormat(NamedTuple):
    language: str
    filter: FileFilter
    check: Callable[[_Context], dict[Path, str]]
    fix: Callable[[_Context], dict[Path, str]]

    @property
    def extensions(self):
        # TODO: b/23842636 - Switch calls of this to using 'filter' and remove.
        return self.filter.endswith


CPP_HEADER_EXTS = cpp.CPP_HEADER_EXTS
CPP_SOURCE_EXTS = cpp.CPP_SOURCE_EXTS
CPP_EXTS = cpp.CPP_EXTS
CPP_FILE_FILTER = cpp.DEFAULT_CPP_FILE_PATTERNS

C_FORMAT = CodeFormat(
    'C and C++', CPP_FILE_FILTER, clang_format_check, clang_format_fix
)

PROTO_FORMAT: CodeFormat = CodeFormat(
    'Protocol buffer',
    DEFAULT_PROTOBUF_FILE_PATTERNS,
    clang_format_check,
    clang_format_fix,
)

JAVA_FORMAT: CodeFormat = CodeFormat(
    'Java',
    DEFAULT_JAVA_FILE_PATTERNS,
    clang_format_check,
    clang_format_fix,
)

JAVASCRIPT_FORMAT: CodeFormat = CodeFormat(
    'JavaScript',
    DEFAULT_JAVASCRIPT_FILE_PATTERNS,
    typescript_format_check,
    typescript_format_fix,
)

TYPESCRIPT_FORMAT: CodeFormat = CodeFormat(
    'TypeScript',
    DEFAULT_TYPESCRIPT_FILE_PATTERNS,
    typescript_format_check,
    typescript_format_fix,
)

# TODO: b/308948504 - Add real code formatting support for CSS
CSS_FORMAT: CodeFormat = CodeFormat(
    'css',
    DEFAULT_CSS_FILE_PATTERNS,
    check_trailing_space,
    fix_trailing_space,
)

GO_FORMAT: CodeFormat = CodeFormat(
    'Go', DEFAULT_GO_FILE_PATTERNS, check_go_format, fix_go_format
)

PYTHON_FORMAT: CodeFormat = CodeFormat(
    'Python',
    DEFAULT_PYTHON_FILE_PATTERNS,
    check_py_format,
    fix_py_format,
)

GN_FORMAT: CodeFormat = CodeFormat(
    'GN', DEFAULT_GN_FILE_PATTERNS, check_gn_format, fix_gn_format
)

BAZEL_FORMAT: CodeFormat = CodeFormat(
    'Bazel',
    DEFAULT_BAZEL_FILE_PATTERNS,
    check_bazel_format,
    fix_bazel_format,
)

COPYBARA_FORMAT: CodeFormat = CodeFormat(
    'Copybara',
    DEFAULT_STARLARK_FILE_PATTERNS,
    check_bazel_format,
    fix_bazel_format,
)

# TODO: b/234881054 - Add real code formatting support for CMake
CMAKE_FORMAT: CodeFormat = CodeFormat(
    'CMake',
    DEFAULT_CMAKE_FILE_PATTERNS,
    check_trailing_space,
    fix_trailing_space,
)

RST_FORMAT: CodeFormat = CodeFormat(
    'reStructuredText',
    DEFAULT_RST_FILE_PATTERNS,
    rst_format_check,
    rst_format_fix,
)

RUST_FORMAT: CodeFormat = CodeFormat(
    'Rust',
    DEFAULT_RUST_FILE_PATTERNS,
    rust_format_check,
    rust_format_fix,
)

MARKDOWN_FORMAT: CodeFormat = CodeFormat(
    'Markdown',
    DEFAULT_MARKDOWN_FILE_PATTERNS,
    check_trailing_space,
    fix_trailing_space,
)

OWNERS_CODE_FORMAT = CodeFormat(
    'OWNERS',
    filter=DEFAULT_OWNERS_FILE_PATTERNS,
    check=check_owners_format,
    fix=fix_owners_format,
)

JSON_FORMAT: CodeFormat = CodeFormat(
    'JSON',
    DEFAULT_JSON_FILE_PATTERNS,
    check=check_json_format,
    fix=fix_json_format,
)

CODE_FORMATS: tuple[CodeFormat, ...] = tuple(
    filter(
        None,
        (
            # keep-sorted: start
            BAZEL_FORMAT,
            CMAKE_FORMAT,
            COPYBARA_FORMAT,
            CSS_FORMAT,
            C_FORMAT,
            GN_FORMAT,
            GO_FORMAT,
            JAVASCRIPT_FORMAT if shutil.which('npm') else None,
            JAVA_FORMAT,
            JSON_FORMAT,
            MARKDOWN_FORMAT,
            OWNERS_CODE_FORMAT,
            PROTO_FORMAT,
            PYTHON_FORMAT,
            RST_FORMAT,
            RUST_FORMAT if shutil.which('rustfmt') else None,
            TYPESCRIPT_FORMAT if shutil.which('npm') else None,
            # keep-sorted: end
        ),
    )
)


# TODO: b/264578594 - Remove these lines when these globals aren't referenced.
CODE_FORMATS_WITH_BLACK: tuple[CodeFormat, ...] = CODE_FORMATS
CODE_FORMATS_WITH_YAPF: tuple[CodeFormat, ...] = CODE_FORMATS


def presubmit_check(
    code_format: CodeFormat,
    *,
    exclude: Collection[str | Pattern[str]] = (),
) -> Callable:
    """Creates a presubmit check function from a CodeFormat object.

    Args:
      exclude: Additional exclusion regexes to apply.
    """

    # Make a copy of the FileFilter and add in any additional excludes.
    file_filter = code_format.filter.concat(exclude=exclude)

    @filter_paths(file_filter=file_filter)
    def check_code_format(ctx: PresubmitContext):
        ctx.paths = presubmit_context.apply_exclusions(ctx)
        errors = findings_to_formatted_diffs(code_format.check(ctx))
        summarize_findings(
            errors,
            log_fix_command=True,
            log_oneliner_summary=True,
        )
        if not errors:
            return

        sio = io.StringIO()
        summarize_findings(
            errors,
            log_fix_command=False,
            log_oneliner_summary=False,
            file=sio,
        )
        ctx.fail(sio.getvalue())

        raise PresubmitFailure

    language = code_format.language.lower().replace('+', 'p').replace(' ', '_')
    check_code_format.name = f'{language}_format'
    check_code_format.doc = f'Check the format of {code_format.language} files.'

    return check_code_format


def presubmit_checks(
    *,
    exclude: Collection[str | Pattern[str]] = (),
    code_formats: Collection[CodeFormat] = CODE_FORMATS,
) -> tuple[Callable, ...]:
    """Returns a tuple with all supported code format presubmit checks.

    Args:
      exclude: Additional exclusion regexes to apply.
      code_formats: A list of CodeFormat objects to run checks with.
    """

    return tuple(presubmit_check(fmt, exclude=exclude) for fmt in code_formats)


class CodeFormatter:
    """Checks or fixes the formatting of a set of files."""

    def __init__(
        self,
        root: Path | None,
        files: Iterable[Path],
        code_formats: Collection[CodeFormat] = CODE_FORMATS_WITH_YAPF,
    ):
        self.root = root
        self._formats: dict[CodeFormat, list] = collections.defaultdict(list)
        self._format_options = FormatOptions.load()
        raw_paths = files
        self.paths: tuple[Path, ...] = self._format_options.filter_paths(files)

        filtered_paths = set(raw_paths) - set(self.paths)
        for path in sorted(filtered_paths):
            _LOG.debug('filtered out %s', path)

        for path in self.paths:
            for code_format in code_formats:
                if code_format.filter.matches(path):
                    _LOG.debug(
                        'Formatting %s as %s', path, code_format.language
                    )
                    self._formats[code_format].append(path)
                    break
            else:
                _LOG.debug('No formatter found for %s', path)

    def _context(self, code_format: CodeFormat):

        return FormatContext(
            root=self.root,
            paths=tuple(self._formats[code_format]),
            format_options=self._format_options,
        )

    def check(self) -> dict[Path, str]:
        """Returns {path: diff} for files with incorrect formatting."""
        errors: dict[Path, str] = {}

        for code_format, files in self._formats.items():
            _LOG.debug('Checking %s', ', '.join(str(f) for f in files))
            errors.update(code_format.check(self._context(code_format)))

        return collections.OrderedDict(sorted(errors.items()))

    def fix(self) -> dict[Path, str]:
        """Fixes format errors for supported files in place."""
        all_errors: dict[Path, str] = {}
        for code_format, files in self._formats.items():
            errors = code_format.fix(self._context(code_format))
            if errors:
                for path, error in errors.items():
                    _LOG.error('Failed to format %s', path)
                    for line in error.splitlines():
                        _LOG.error('%s', line)
                all_errors.update(errors)
                continue

            _LOG.info(
                'Formatted %s', plural(files, code_format.language + ' file')
            )
        return all_errors


def _file_summary(files: Iterable[Path | str], base: Path) -> list[str]:
    try:
        return file_summary(
            Path(f).resolve().relative_to(base.resolve()) for f in files
        )
    except ValueError:
        return []


def format_paths_in_repo(
    paths: Collection[Path | str],
    exclude: Collection[Pattern[str]],
    apply_fixes: bool,
    base: str,
    code_formats: Collection[CodeFormat] = CODE_FORMATS,
    jobs: int = 1,  # pylint: disable=unused-argument
    directory: Path | None = None,
) -> int:
    """Checks or fixes formatting for files in a Git repo."""
    if directory:
        os.chdir(directory)

    repo = git_repo.within_repo()

    files = collect_files_in_current_repo(
        paths,
        PresubmitToolRunner(),
        modified_since_git_ref=base,
        exclude_patterns=exclude,
        action_flavor_text='Formatting',
    )

    # The format tooling currently expects absolute paths when filtering paths.
    files = [Path.cwd() / f for f in files]

    return format_files(
        files,
        apply_fixes,
        repo=repo,
        code_formats=code_formats,
    )


def format_files(
    paths: Collection[Path | str],
    apply_fixes: bool,
    repo: Path | None = None,
    code_formats: Collection[CodeFormat] = CODE_FORMATS,
) -> int:
    """Checks or fixes formatting for the specified files."""

    root: Path | None = git_repo.within_repo()
    if root is None and paths:
        parent = Path(next(iter(paths))).parent
        root = git_repo.within_repo(parent)

    formatter = CodeFormatter(
        files=(Path(p) for p in paths),
        code_formats=code_formats,
        root=root,
    )

    _LOG.info('Checking formatting for %s', plural(formatter.paths, 'file'))

    for line in _file_summary(paths, repo if repo else Path.cwd()):
        print(line, file=sys.stderr)

    check_errors = findings_to_formatted_diffs(formatter.check())
    summarize_findings(
        check_errors,
        log_fix_command=(not apply_fixes),
        log_oneliner_summary=True,
    )

    if check_errors:
        if apply_fixes:
            _LOG.info(
                'Applying formatting fixes to %d files', len(check_errors)
            )
            fix_errors = findings_to_formatted_diffs(formatter.fix())
            if fix_errors:
                _LOG.info('Failed to apply formatting fixes')
                summarize_findings(
                    check_errors,
                    log_fix_command=False,
                    log_oneliner_summary=True,
                )
                return 1

            _LOG.info('Formatting fixes applied successfully')
            return 0

        _LOG.error('Formatting errors found')
        return 1

    _LOG.info('Congratulations! No formatting changes needed')
    return 0


def arguments(
    git_paths: bool = True,  # pylint: disable=unused-argument
) -> argparse.ArgumentParser:
    """Creates an argument parser for format_files or format_paths_in_repo."""

    parser = argparse.ArgumentParser(description=__doc__)

    add_arguments(parser, default_to_fix=False)

    return parser


def main() -> int:
    """Check and fix formatting for source files."""
    return format_paths_in_repo(**vars(arguments().parse_args()))


if __name__ == '__main__':
    try:
        # If pw_cli is available, use it to initialize logs.
        from pw_cli import log  # pylint: disable=ungrouped-imports

        log.install(logging.INFO)
    except ImportError:
        # If pw_cli isn't available, display log messages like a simple print.
        logging.basicConfig(format='%(message)s', level=logging.INFO)

    sys.exit(main())
