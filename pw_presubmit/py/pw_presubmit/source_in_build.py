# Copyright 2023 The Pigweed Authors
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
"""Checks that source files are listed in build files, such as BUILD.bazel."""

import logging
from pathlib import Path
from typing import Callable, Sequence

from pw_cli.file_filter import FileFilter
from pw_presubmit import build, format_code, git_repo
from pw_presubmit.presubmit import Check, filter_paths
from pw_presubmit.presubmit_context import PresubmitContext
from pw_presubmit.private.result import PresubmitFailure

_LOG: logging.Logger = logging.getLogger(__name__)

# The filter is used twice for each source_is_in_* check.  First to decide
# whether the check should be run. Once it's running, we use ctx.all_paths
# instead of ctx.paths since we want to check that all files are in the build,
# not just changed files, but we need to run ctx.all_paths through the same
# filter within the check or we won't properly ignore files that the caller
# asked to be ignored.

_DEFAULT_BAZEL_EXTENSIONS = (
    '.py',
    '.rst',
    *format_code.C_FORMAT.extensions,
)


def bazel(
    source_filter: FileFilter,
    files_and_extensions_to_check: Sequence[str] = _DEFAULT_BAZEL_EXTENSIONS,
) -> Check:
    """Create a presubmit check that ensures source files are in Bazel files.

    Args:
        source_filter: filter that selects files that must be in the Bazel build
        files_and_extensions_to_check: files and extensions to look for (the
            source_filter might match build files that won't be in the build but
            this should only match source files)
    """

    @filter_paths(file_filter=source_filter)
    def source_is_in_bazel_build(ctx: PresubmitContext):
        """Checks that source files are in the Bazel build."""

        missing = build.check_bazel_build_for_files(
            files_and_extensions_to_check,
            ctx.paths,
            bazel_dirs=[ctx.root],
        )

        if missing:
            ctx.fail(
                'Missing files:\n' + '\n'.join(str(miss) for miss in missing)
            )

            _LOG.warning('All source files must appear in BUILD.bazel files')
            raise PresubmitFailure

    return source_is_in_bazel_build


_DEFAULT_GN_EXTENSIONS = (
    'setup.cfg',
    '.toml',
    '.rst',
    '.py',
    *format_code.C_FORMAT.extensions,
)


def gn(  # pylint: disable=invalid-name
    source_filter: FileFilter,
    files_and_extensions_to_check: Sequence[str] = _DEFAULT_GN_EXTENSIONS,
) -> Check:
    """Create a presubmit check that ensures source files are in GN files.

    Args:
        source_filter: filter that selects files that must be in the GN build
        files_and_extensions_to_check: files and extensions to look for (the
            source_filter might match build files that won't be in the build but
            this should only match source files)
    """

    @filter_paths(file_filter=source_filter)
    def source_is_in_gn_build(ctx: PresubmitContext):
        """Checks that source files are in the GN build."""

        missing = build.check_gn_build_for_files(
            files_and_extensions_to_check,
            ctx.paths,
            gn_build_files=git_repo.find_git_repo(ctx.root).list_files(
                pathspecs=['BUILD.gn', '*BUILD.gn']
            ),
        )

        if missing:
            ctx.fail(
                'Missing files:\n' + '\n'.join(str(miss) for miss in missing)
            )

            _LOG.warning('All source files must appear in BUILD.gn files')
            raise PresubmitFailure

    return source_is_in_gn_build


_DEFAULT_CMAKE_EXTENSIONS = (*format_code.C_FORMAT.extensions,)


def cmake(
    source_filter: FileFilter,
    run_cmake: Callable[[PresubmitContext], None],
    files_and_extensions_to_check: Sequence[str] = _DEFAULT_CMAKE_EXTENSIONS,
) -> Check:
    """Create a presubmit check that ensures source files are in CMake files.

    Args:
        source_filter: filter that selects files that must be in the CMake build
        run_cmake: callable that takes a PresubmitContext and invokes CMake
        files_and_extensions_to_check: files and extensions to look for (the
            source_filter might match build files that won't be in the build but
            this should only match source files)
    """

    to_check = tuple(files_and_extensions_to_check)

    @filter_paths(file_filter=source_filter)
    def source_is_in_cmake_build(ctx: PresubmitContext):
        """Checks that source files are in the CMake build."""

        run_cmake(ctx)
        missing = build.check_compile_commands_for_files(
            ctx.output_dir / 'compile_commands.json',
            (f for f in ctx.paths if str(f).endswith(to_check)),
        )

        if missing:
            ctx.fail(
                'Missing files:\n' + '\n'.join(str(miss) for miss in missing)
            )

            _LOG.warning(
                'Files missing from CMake:\n%s',
                '\n'.join(str(f) for f in missing),
            )
            raise PresubmitFailure

    return source_is_in_cmake_build


_DEFAULT_SOONG_EXTENSIONS = (*format_code.CPP_SOURCE_EXTS, '.proto')


def soong(  # pylint: disable=invalid-name
    source_filter: FileFilter,
    files_and_extensions_to_check: Sequence[str] = _DEFAULT_SOONG_EXTENSIONS,
) -> Check:
    """Create a presubmit check that ensures sources are in Android.bp files.

    Args:
        source_filter: filter that selects files that must be in the Soong files
        files_and_extensions_to_check: files and extensions to look for (the
            source_filter might match build files that won't be in the build but
            this should only match source files)
    """

    @filter_paths(file_filter=source_filter)
    def source_is_in_soong_build(ctx: PresubmitContext):
        """Checks that source files are in the Soong build."""

        # For now, only check modules where there is an Android.bp file.
        relevant_paths: list[Path] = []
        for path in ctx.paths:
            # For now, don't require tests be included in Android.bp files.
            split = path.stem.split('_')
            if 'test' in split or 'mock' in split:
                continue

            if (path.parent / 'Android.bp').is_file():
                relevant_paths.append(path)

        missing = build.check_soong_build_for_files(
            files_and_extensions_to_check,
            relevant_paths,
            soong_build_files=git_repo.find_git_repo(ctx.root).list_files(
                pathspecs=['Android.bp', '*Android.bp']
            ),
        )

        if missing:
            ctx.fail(
                'Missing files:\n' + '\n'.join(str(miss) for miss in missing)
            )

            _LOG.warning('All source files must appear in Android.bp files')
            raise PresubmitFailure

    return source_is_in_soong_build
