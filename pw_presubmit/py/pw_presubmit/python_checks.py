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
"""Preconfigured checks for Python code.

These checks assume that they are running in a preconfigured Python environment.
"""

import difflib
import logging
from pathlib import Path
import platform
import re
import shutil
import sys
from tempfile import TemporaryDirectory
import venv

from pw_cli.diff import colorize_diff_line

from pw_presubmit.presubmit import (
    call,
    filter_paths,
)
from pw_presubmit.private.result import PresubmitFailure
from pw_presubmit.git_repo import LoggingGitRepo
from pw_presubmit.presubmit_context import PresubmitContext
from pw_presubmit import build, git_repo
from pw_presubmit.tools import log_run

_LOG = logging.getLogger(__name__)

_PYTHON_EXTENSIONS = ('.py', '.gn', '.gni')

_PYTHON_PACKAGE_EXTENSIONS = (
    'setup.cfg',
    'constraint.list',
    'requirements.txt',
)

_PYTHON_IS_3_9_OR_HIGHER = sys.version_info >= (
    3,
    9,
)


@filter_paths(endswith=_PYTHON_EXTENSIONS)
def gn_python_check(ctx: PresubmitContext):
    """Runs gn gen and ninja for Python tests and lint."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'python.tests', 'python.lint')


def _transform_lcov_file_paths(lcov_file: Path, repo_root: Path) -> str:
    """Modify file paths in an lcov file to be relative to the repo root.

    See `man geninfo` for info on the lcov format."""

    lcov_input = lcov_file.read_text()
    lcov_output = ''

    if not _PYTHON_IS_3_9_OR_HIGHER:
        return lcov_input

    for line in lcov_input.splitlines():
        if not line.startswith('SF:'):
            lcov_output += line + '\n'
            continue

        # Get the file path after SF:
        file_string = line[3:].rstrip()
        source_file_path = Path(file_string)

        # Attempt to map a generated Python package source file to the root
        # source tree.
        # pylint: disable=no-member
        if not source_file_path.is_relative_to(
            repo_root  # type: ignore[attr-defined]
        ):
            # pylint: enable=no-member
            source_file_path = repo_root / str(source_file_path).replace(
                'python/gen/', ''
            ).replace('py.generated_python_package/', '')

        # If mapping fails don't modify this line.
        # pylint: disable=no-member
        if not source_file_path.is_relative_to(
            repo_root  # type: ignore[attr-defined]
        ):
            # pylint: enable=no-member
            lcov_output += line + '\n'
            continue

        source_file_path = source_file_path.relative_to(repo_root)
        lcov_output += f'SF:{source_file_path}\n'

    return lcov_output


@filter_paths(endswith=_PYTHON_EXTENSIONS)
def gn_python_test_coverage(ctx: PresubmitContext):
    """Run Python tests with coverage and create reports."""
    build.gn_gen(ctx, pw_build_PYTHON_TEST_COVERAGE=True)
    build.ninja(ctx, 'python.tests')

    # Find coverage data files
    coverage_data_files = list(ctx.output_dir.glob('**/*.coverage'))
    if not coverage_data_files:
        return

    # Merge coverage data files to out/.coverage
    call(
        'coverage',
        'combine',
        # Leave existing coverage files in place; by default they are deleted.
        '--keep',
        *coverage_data_files,
        cwd=ctx.output_dir,
    )
    combined_data_file = ctx.output_dir / '.coverage'
    _LOG.info('Coverage data saved to: %s', combined_data_file.resolve())

    # Always ignore generated proto python and setup.py files.
    coverage_omit_patterns = '--omit=*_pb2.py,*/setup.py'

    # Output coverage percentage summary to the terminal of changed files.
    changed_python_files = list(
        str(p) for p in ctx.paths if str(p).endswith('.py')
    )
    report_args = [
        'coverage',
        'report',
        '--ignore-errors',
        coverage_omit_patterns,
    ]
    report_args += changed_python_files
    log_run(report_args, check=False, cwd=ctx.output_dir)

    # Generate a json report
    call('coverage', 'lcov', coverage_omit_patterns, cwd=ctx.output_dir)
    lcov_data_file = ctx.output_dir / 'coverage.lcov'
    lcov_data_file.write_text(
        _transform_lcov_file_paths(lcov_data_file, repo_root=ctx.root)
    )
    _LOG.info('Coverage lcov saved to: %s', lcov_data_file.resolve())

    # Generate an html report
    call('coverage', 'html', coverage_omit_patterns, cwd=ctx.output_dir)
    html_report = ctx.output_dir / 'htmlcov' / 'index.html'
    _LOG.info('Coverage html report saved to: %s', html_report.resolve())


@filter_paths(endswith=_PYTHON_PACKAGE_EXTENSIONS)
def vendor_python_wheels(ctx: PresubmitContext) -> None:
    """Download Python packages locally for the current platform."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'pip_vendor_wheels')
    # Use the upstream-only build venv.
    build_venv_target_name = 'upstream_pigweed_build_venv'

    download_log = (
        ctx.output_dir
        / f'python/gen/pw_env_setup/{build_venv_target_name}.vendor_wheels'
        / 'pip_download_log.txt'
    )
    _LOG.info('Python package download log: %s', download_log)

    wheel_output = (
        ctx.output_dir
        / 'python/gen/pw_env_setup'
        / f'{build_venv_target_name}.vendor_wheels/wheels/'
    )
    wheel_destination = ctx.output_dir / 'python_wheels'
    shutil.rmtree(wheel_destination, ignore_errors=True)
    shutil.copytree(wheel_output, wheel_destination, dirs_exist_ok=True)

    _LOG.info('Python packages downloaded to: %s', wheel_destination)


SETUP_CFG_VERSION_REGEX = re.compile(
    r'^version = (?P<version>'
    r'(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)'
    r')$',
    re.MULTILINE,
)


def _find_existing_setup_cfg_version(setup_cfg: Path) -> re.Match:
    version_match = SETUP_CFG_VERSION_REGEX.search(setup_cfg.read_text())
    if not version_match:
        raise PresubmitFailure(
            f'Unable to find "version = x.x.x" line in {setup_cfg}'
        )
    return version_match


def _version_bump_setup_cfg(
    repo_root: Path,
    setup_cfg: Path,
) -> str:
    """Increment the version patch number of a setup.cfg

    Skips modifying if there are modifications since origin/main.

    Returns:
       The version number as a string
    """
    repo = LoggingGitRepo(repo_root)
    setup_cfg = repo_root / setup_cfg

    # Grab the current version string.
    _LOG.info('Checking the version patch number in: %s', setup_cfg)
    setup_cfg_text = setup_cfg.read_text()
    version_match = _find_existing_setup_cfg_version(setup_cfg)
    _LOG.info('Found: %s', version_match[0])
    version_number = version_match['version']

    # Skip modifying the version if it is different compared to origin/main.
    modified_files = repo.list_files(commit='origin/main')
    modify_setup_cfg = True
    if setup_cfg in modified_files:
        # Don't update the file
        modify_setup_cfg = False
        _LOG.warning(
            '%s is already modified, skipping version update.', setup_cfg
        )

    if modify_setup_cfg:
        # Increment the patch number.
        try:
            patch_number = int(version_match['patch']) + 1
        except ValueError as err:
            raise PresubmitFailure(
                f"Unable to increment patch number: '{version_match['patch']}' "
                f"for version line: '{version_match[0]}'"
            ) from err

        version_number = (
            f"{version_match['major']}.{version_match['minor']}.{patch_number}"
        )
        new_line = f'version = {version_number}'
        new_text = SETUP_CFG_VERSION_REGEX.sub(
            new_line,
            setup_cfg_text,
            count=1,
        )

        # Print the diff
        setup_cfg_diff = list(
            difflib.unified_diff(
                setup_cfg_text.splitlines(),
                new_text.splitlines(),
                fromfile=str(setup_cfg) + ' (original)',
                tofile=str(setup_cfg) + ' (updated)',
                lineterm='',
                n=1,
            )
        )
        if setup_cfg_diff:
            for line in setup_cfg_diff:
                print(colorize_diff_line(line))

        # Update the file.
        setup_cfg.write_text(new_text, encoding='utf-8')

    return version_number


def version_bump_pigweed_pypi_distribution(ctx: PresubmitContext) -> None:
    """Update the version patch number in //pw_env_setup/pypi_common_setup.cfg

    This presubmit creates a new git branch, updates the version and makes a new
    commit with the standard version bump subject line.
    """
    repo = LoggingGitRepo(ctx.root)

    # Check there are no uncommitted changes.
    modified_files = repo.list_files(commit='HEAD')
    if modified_files:
        raise PresubmitFailure('There must be no modified files to proceed.')

    # Checkout a new branch for the version bump. Resets an existing branch if
    # it already exists.
    log_run(
        [
            'git',
            'checkout',
            '-B',
            'pypi-version-bump',
            '--no-track',
            'origin/main',
        ],
        check=True,
        cwd=ctx.root,
    )

    # Update the version number.
    setup_cfg = 'pw_env_setup/pypi_common_setup.cfg'
    version_number = _version_bump_setup_cfg(
        repo_root=ctx.root,
        setup_cfg=ctx.root / 'pw_env_setup/pypi_common_setup.cfg',
    )

    # Add and commit changes.
    log_run(['git', 'add', setup_cfg], check=True, cwd=ctx.root)
    git_commit_args = [
        'git',
        'commit',
        '-m',
        f'pw_env_setup: PyPI version bump to {version_number}',
    ]
    log_run(git_commit_args, check=True, cwd=ctx.root)
    _LOG.info('Version bump commit created in branch "pypi-version-bump"')
    _LOG.info('Upload with:\n  git push origin HEAD:refs/for/main')


def upload_pigweed_pypi_distribution(
    ctx: PresubmitContext,
) -> None:
    """Upload the pigweed pypi distribution to pypi.org.

    This requires an API token to be setup for the current user. See also:
    https://pypi.org/help/#apitoken
    https://packaging.python.org/en/latest/guides/distributing-packages-using-setuptools/#create-an-account
    """
    version_match = _find_existing_setup_cfg_version(
        Path(ctx.root / 'pw_env_setup/pypi_common_setup.cfg'),
    )
    version_number = version_match['version']

    _LOG.info('Cleaning any existing build artifacts.')
    build.gn_gen(ctx)

    dist_output_path = (
        ctx.output_dir
        / 'python/obj/pw_env_setup/pypi_pigweed_python_source_tree'
    )

    # Always remove any existing build artifacts before generating a
    # new distribution. 'python -m build' leaves a 'dist' directory without
    # cleaning up.
    shutil.rmtree(dist_output_path, ignore_errors=True)
    build.ninja(ctx, 'pigweed_pypi_distribution', '-t', 'clean')

    # Generate the distribution
    _LOG.info('Running the ninja build.')
    build.ninja(ctx, 'pigweed_pypi_distribution')

    # Check the output is in the right place.
    if any(
        not (dist_output_path / f).is_file() for f in ['README.md', 'LICENSE']
    ):
        raise PresubmitFailure(
            f'Missing pypi distribution files in: {dist_output_path}'
        )

    # Create a new venv for building and uploading.
    venv_path = ctx.output_dir / 'pypi_upload_venv'
    _LOG.info('Creating venv for uploading in: %s', venv_path)
    shutil.rmtree(venv_path, ignore_errors=True)
    venv.create(venv_path, symlinks=True, with_pip=True)
    py_bin = venv_path / 'bin/python'

    # Install upload tools.
    _LOG.info('Running: pip install --upgrade pip %s', venv_path)
    log_run(
        [py_bin, '-m', 'pip', 'install', '--upgrade', 'pip'],
        check=True,
        cwd=ctx.output_dir,
    )
    _LOG.info('Running: pip install --upgrade build twine %s', venv_path)
    log_run(
        [py_bin, '-m', 'pip', 'install', '--upgrade', 'build', 'twine'],
        check=True,
        cwd=ctx.output_dir,
    )

    # Create upload artifacts
    _LOG.info('Running: python -m build')
    log_run([py_bin, '-m', 'build'], check=True, cwd=dist_output_path)

    dist_path = dist_output_path / 'dist'
    upload_files = sorted(dist_path.glob('*'))
    expected_files = sorted(
        [
            dist_path / f'pigweed-{version_number}.tar.gz',
            dist_path / f'pigweed-{version_number}-py3-none-any.whl',
        ]
    )
    if upload_files != expected_files:
        raise PresubmitFailure(
            'Unexpected dist files found for upload. Skipping upload.\n'
            f'Found:\n {upload_files}\n'
            f'Expected:\n {expected_files}\n'
        )

    # Upload to pypi.org
    upload_args = [py_bin, '-m', 'twine', 'upload']
    upload_args.extend(expected_files)
    log_run(upload_args, check=True, cwd=dist_output_path)


def _generate_constraint_with_hashes(
    ctx: PresubmitContext, input_file: Path, output_file: Path
) -> None:
    assert input_file.is_file()

    call(
        "pip-compile",
        input_file,
        "--generate-hashes",
        "--reuse-hashes",
        "--resolver=backtracking",
        "--strip-extras",
        # Force pinning pip and setuptools
        "--allow-unsafe",
        "-o",
        output_file,
    )

    # Remove absolute paths from comments
    output_text = output_file.read_text()
    output_text = output_text.replace(str(ctx.output_dir), '')
    output_text = output_text.replace(str(ctx.root), '')
    output_text = output_text.replace(str(output_file.parent), '')

    # Regex for pip requirement comment. Matches:
    #
    # ^    # via -r some/prefix/path/compiled_requirements.txt$
    # ^    # -r some/prefix/path/compiled_requirements.txt$
    # ^    #    -r some/prefix/path/compiled_requirements.txt$
    #
    dash_r_comment_regex = re.compile(
        r'^(?P<comment>'
        r' *# *'
        r'(?:via)?'
        r' *-r)'
        r'.*compiled_requirements.txt'
    )

    final_output_text = ''
    for line in output_text.splitlines(keepends=True):
        # Remove --find-links lines
        if line.startswith('--find-links'):
            continue
        # Cleanup path prefixes in comments for compiled_requirements.txt files.
        if 'compiled_requirements.txt' in line:
            final_output_text += dash_r_comment_regex.sub(
                r'\g<comment> compiled_requirements.txt', line, count=1
            )
            continue
        # Remove blank lines
        if line == '\n':
            continue
        final_output_text += line

    output_file.write_text(final_output_text)


def _update_upstream_python_constraints(
    ctx: PresubmitContext,
    update_files: bool = False,
    diff_in_place: bool = False,
) -> None:
    """Regenerate platform specific Python constraint files with hashes."""
    with TemporaryDirectory() as tmpdirname:
        out_dir = Path(tmpdirname)

        # Generate constraint_hashes_{linux,darwin,windows}.list
        # Use the default downstream build venv.
        build_venv_target_name = 'pigweed_build_venv'
        build.gn_gen(
            ctx,
            # Omit the upstream only requirements for downstream constraint
            # hashes.
            pw_build_PIP_REQUIREMENTS=[],
            # Use the constraint file without hashes as the input. This is where
            # new packages are added by developers.
            pw_build_PIP_CONSTRAINTS=[
                '//pw_env_setup/py/pw_env_setup/virtualenv_setup/'
                'constraint.list',
            ],
            pw_build_PYTHON_BUILD_VENV=(
                f'//pw_env_setup:{build_venv_target_name}'
            ),
            # This should always be set to false when regenrating constraints.
            pw_build_PYTHON_PIP_INSTALL_REQUIRE_HASHES=False,
        )
        build.ninja(ctx, 'pip_constraint_update')

        # Either: darwin, linux or windows
        platform_name = platform.system().lower()

        constraint_hashes_filename = f'constraint_hashes_{platform_name}.list'
        constraint_hashes_original = (
            ctx.root
            / 'pw_env_setup/py/pw_env_setup/virtualenv_setup'
            / constraint_hashes_filename
        )
        constraint_hashes_tmp_out = out_dir / constraint_hashes_filename
        _generate_constraint_with_hashes(
            ctx,
            input_file=(
                ctx.output_dir
                / f'python/gen/pw_env_setup/{build_venv_target_name}'
                / 'compiled_requirements.txt'
            ),
            output_file=constraint_hashes_tmp_out,
        )

        # Generate upstream_requirements_{linux,darwin,windows}_lock.txt
        # Use the upstream-only build venv.
        build_venv_target_name = 'upstream_pigweed_build_venv'
        build.gn_gen(
            ctx,
            # Include upstream only requirements.
            pw_build_PIP_REQUIREMENTS=[
                '//pw_env_setup/py/pw_env_setup/virtualenv_setup/'
                'pigweed_upstream_requirements.txt'
            ],
            # Use the constraint file without hashes as the input. This is where
            # new packages are added by developers.
            pw_build_PIP_CONSTRAINTS=[
                '//pw_env_setup/py/pw_env_setup/virtualenv_setup/'
                'constraint.list',
            ],
            pw_build_PYTHON_BUILD_VENV=(
                f'//pw_env_setup:{build_venv_target_name}'
            ),
            # This should always be set to false when regenrating constraints.
            pw_build_PYTHON_PIP_INSTALL_REQUIRE_HASHES=False,
        )
        build.ninja(ctx, 'pip_constraint_update')

        upstream_requirements_lock_filename = (
            f'upstream_requirements_{platform_name}_lock.txt'
        )
        upstream_requirements_lock_original = (
            ctx.root
            / 'pw_env_setup/py/pw_env_setup/virtualenv_setup'
            / upstream_requirements_lock_filename
        )
        upstream_requirements_lock_tmp_out = (
            out_dir / upstream_requirements_lock_filename
        )
        _generate_constraint_with_hashes(
            ctx,
            input_file=(
                ctx.output_dir
                / f'python/gen/pw_env_setup/{build_venv_target_name}'
                / 'compiled_requirements.txt'
            ),
            output_file=upstream_requirements_lock_tmp_out,
        )

        if update_files:
            constraint_hashes_original.write_text(
                constraint_hashes_tmp_out.read_text()
            )
            _LOG.info('Updated: %s', constraint_hashes_original)
            upstream_requirements_lock_original.write_text(
                upstream_requirements_lock_tmp_out.read_text()
            )
            _LOG.info('Updated: %s', upstream_requirements_lock_original)

            if diff_in_place:
                diff = git_repo.find_git_repo(ctx.root).diff(
                    '--',
                    constraint_hashes_original,
                    upstream_requirements_lock_original,
                )
                if not diff:
                    return

                ctx.output_dir.mkdir(exist_ok=True, parents=True)
                diff_path = ctx.output_dir / 'git_diff.txt'
                with open(diff_path, 'w') as stdout:
                    stdout.write(diff)

                raise PresubmitFailure(
                    f'git diff output is not empty; see {diff_path}'
                )

            return

        # Make a diff of required changes
        constraint_hashes_diff = list(
            difflib.unified_diff(
                constraint_hashes_original.read_text(
                    'utf-8', errors='replace'
                ).splitlines(),
                constraint_hashes_tmp_out.read_text(
                    'utf-8', errors='replace'
                ).splitlines(),
                fromfile=str(constraint_hashes_original) + ' (original)',
                tofile=str(constraint_hashes_original) + ' (updated)',
                lineterm='',
                n=1,
            )
        )
        upstream_requirements_lock_diff = list(
            difflib.unified_diff(
                upstream_requirements_lock_original.read_text(
                    'utf-8', errors='replace'
                ).splitlines(),
                upstream_requirements_lock_tmp_out.read_text(
                    'utf-8', errors='replace'
                ).splitlines(),
                fromfile=str(upstream_requirements_lock_original)
                + ' (original)',
                tofile=str(upstream_requirements_lock_original) + ' (updated)',
                lineterm='',
                n=1,
            )
        )
        if constraint_hashes_diff:
            for line in constraint_hashes_diff:
                print(colorize_diff_line(line))
        if upstream_requirements_lock_diff:
            for line in upstream_requirements_lock_diff:
                print(colorize_diff_line(line))
        if constraint_hashes_diff or upstream_requirements_lock_diff:
            raise PresubmitFailure(
                'Please run:\n'
                '\n'
                '  pw presubmit --step update_upstream_python_constraints'
            )


@filter_paths(endswith=_PYTHON_PACKAGE_EXTENSIONS)
def check_upstream_python_constraints(ctx: PresubmitContext) -> None:
    _update_upstream_python_constraints(ctx, update_files=False)


@filter_paths(endswith=_PYTHON_PACKAGE_EXTENSIONS)
def update_upstream_python_constraints(ctx: PresubmitContext) -> None:
    _update_upstream_python_constraints(ctx, update_files=True)


def diff_upstream_python_constraints(ctx: PresubmitContext) -> None:
    _update_upstream_python_constraints(
        ctx, update_files=True, diff_in_place=True
    )


@filter_paths(endswith=_PYTHON_EXTENSIONS + ('.pylintrc',))
def gn_python_lint(ctx: PresubmitContext) -> None:
    build.gn_gen(ctx)
    build.ninja(ctx, 'python.lint')
