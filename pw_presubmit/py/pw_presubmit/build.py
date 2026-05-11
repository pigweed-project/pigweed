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
"""Functions for building code during presubmit checks."""

import base64
import contextlib
import io
import itertools
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import (
    Collection,
    Container,
    Iterable,
    Mapping,
    Set,
)

from pw_build.build_recipe import gn_args
from pw_cli.plural import plural
from pw_cli.file_filter import FileFilter
from pw_presubmit.presubmit import (
    call,
    filter_paths,
)
from pw_presubmit.private.check import (
    Check,
)
from pw_presubmit.private.result import (
    PresubmitFailure,
)
from pw_presubmit.presubmit_context import (
    LuciContext,
    PresubmitContext,
)

__all__ = [
    'gn_gen',
    'ninja',
    'gn_check',
    'gn_args',
    'CoverageOptions',
    'CommonCoverageOptions',
    'CodeSearchCoverageOptions',
    'GerritCoverageOptions',
]

from pw_presubmit.private.build_common import (
    gn_gen,
    ninja,
    gn_check,
    CoverageOptions,
    CommonCoverageOptions,
    CodeSearchCoverageOptions,
    GerritCoverageOptions,
)

from pw_presubmit import (
    bazel_parser,
    format_code,
)
from pw_presubmit.tools import (
    log_run,
    format_command,
)

# Import GnGenNinja for backwards compatibility. Bazel never needs GnGenNinja;
# it is not a dep and the import will fail, which is fine.
try:
    from pw_presubmit.build_with_pw_package import GnGenNinja  # type: ignore # pylint: disable=unused-import
except ModuleNotFoundError:
    pass

_LOG = logging.getLogger(__name__)


BAZEL_EXECUTABLE = 'bazel'


def _get_remote_instance_name(ctx_luci: LuciContext) -> str:
    instance_name = ''
    if ctx_luci.project == 'pigweed':
        instance_name = 'pigweed-rbe-open'
    else:
        instance_name = 'pigweed-rbe-private'
    if ctx_luci.is_try:
        instance_name += '-pre'

    # pylint: disable-next=line-too-long
    return f'--remote_instance_name=projects/{instance_name}/instances/default-instance'


def bazel(
    ctx: PresubmitContext,
    cmd: str,
    *args: str,
    remote_download_outputs: str = 'minimal',
    stdout: io.TextIOWrapper | None = None,
    strict_module_lockfile: bool = False,
    use_remote_cache: bool = False,
    **kwargs,
) -> None:
    """Invokes Bazel with some common flags set.

    Intended for use with bazel build and test. May not work with others.
    """

    num_jobs: list[str] = []
    if ctx.num_jobs is not None:
        num_jobs.extend(('--jobs', str(ctx.num_jobs)))

    keep_going: list[str] = []
    if ctx.continue_after_build_error:
        keep_going.append('--keep_going')

    strict_lockfile: list[str] = []
    if strict_module_lockfile:
        strict_lockfile.append('--lockfile_mode=error')

    remote_cache: list[str] = []
    if use_remote_cache and ctx.luci:
        remote_cache.append('--config=remote_cache')
        remote_cache.append('--remote_upload_local_results=true')
        remote_cache.append(_get_remote_instance_name(ctx.luci))
        remote_cache.append(
            f'--remote_download_outputs={remote_download_outputs}'
        )

    symlink_prefix: list[str] = []
    if cmd not in ('mod', 'query'):
        # bazel query and bazel mod don't support the --symlink_prefix flag.
        symlink_prefix.append(f'--symlink_prefix={ctx.output_dir / "bazel-"}')

    ctx.output_dir.mkdir(exist_ok=True, parents=True)
    try:
        with contextlib.ExitStack() as stack:
            if not stdout:
                stdout = stack.enter_context(
                    (ctx.output_dir / f'bazel.{cmd}.stdout').open('w')
                )

            with (
                (ctx.output_dir / 'bazel.output.base').open('w') as outs,
                (ctx.output_dir / 'bazel.output.base.err').open('w') as errs,
            ):
                call(
                    BAZEL_EXECUTABLE,
                    'info',
                    'output_base',
                    tee=outs,
                    stderr=errs,
                )

            call(
                BAZEL_EXECUTABLE,
                cmd,
                *symlink_prefix,
                *num_jobs,
                *keep_going,
                *strict_lockfile,
                *remote_cache,
                *args,
                cwd=ctx.root,
                tee=stdout,
                call_annotation={'build_system': 'bazel'},
                **kwargs,
            )

    except PresubmitFailure as exc:
        if stdout:
            failure = bazel_parser.parse_bazel_stdout(Path(stdout.name))
            if failure:
                ctx.fail(failure)

        raise exc


def get_gn_args(directory: Path) -> list[dict[str, dict[str, str]]]:
    """Dumps GN variables to JSON."""
    proc = log_run(
        ['gn', 'args', directory, '--list', '--json'], stdout=subprocess.PIPE
    )
    return json.loads(proc.stdout)


def cmake(
    ctx: PresubmitContext,
    *args: str,
    env: Mapping['str', 'str'] | None = None,
) -> None:
    """Runs CMake for Ninja on the given source and output directories."""
    call(
        'cmake',
        '-B',
        ctx.output_dir,
        '-S',
        ctx.root,
        '-G',
        'Ninja',
        *args,
        env=env,
    )


def env_with_clang_vars() -> Mapping[str, str]:
    """Returns the environment variables with CC, CXX, etc. set for clang."""
    env = os.environ.copy()
    env['CC'] = env['LD'] = env['AS'] = 'clang'
    env['CXX'] = 'clang++'
    return env


def _get_paths_from_command(source_dir: Path, *args, **kwargs) -> Set[Path]:
    """Runs a command and reads Bazel or GN //-style paths from it."""
    process = log_run(args, capture_output=True, cwd=source_dir, **kwargs)

    if process.returncode:
        _LOG.error(
            'Build invocation failed with return code %d!', process.returncode
        )
        _LOG.error(
            '[COMMAND] %s\n%s\n%s',
            *format_command(args, kwargs),
            process.stderr.decode(),
        )
        raise PresubmitFailure

    files = set()

    for line in process.stdout.splitlines():
        path = line.strip().lstrip(b'/').replace(b':', b'/').decode()
        path = source_dir.joinpath(path)
        if path.is_file():
            files.add(path)

    return files


# Finds string literals with '.' in them.
_MAYBE_A_PATH = re.compile(
    r'"'  # Starting double quote.
    # Start capture group 1 - the whole filename:
    #   File basename, a single period, file extension.
    r'([^\n" ]+\.[^\n" ]+)'
    # Non-capturing group 2 (optional).
    r'(?: > [^\n"]+)?'  # pw_zip style string "input_file.txt > output_file.txt"
    r'"'  # Ending double quote.
)


def _search_files_for_paths(build_files: Iterable[Path]) -> Iterable[Path]:
    for build_file in build_files:
        directory = build_file.parent

        for string in _MAYBE_A_PATH.finditer(build_file.read_text()):
            path = directory / string.group(1)
            if path.is_file():
                yield path


def _read_compile_commands(compile_commands: Path) -> dict:
    with compile_commands.open('rb') as fd:
        return json.load(fd)


def compiled_files(compile_commands: Path) -> Iterable[Path]:
    for command in _read_compile_commands(compile_commands):
        file = Path(command['file'])
        if file.is_absolute():
            yield file
        else:
            yield file.joinpath(command['directory']).resolve()


def check_compile_commands_for_files(
    compile_commands: Path | Iterable[Path],
    files: Iterable[Path],
    extensions: Collection[str] = format_code.CPP_SOURCE_EXTS,
) -> list[Path]:
    """Checks for paths in one or more compile_commands.json files.

    Only checks C and C++ source files by default.
    """
    if isinstance(compile_commands, Path):
        compile_commands = [compile_commands]

    compiled = frozenset(
        itertools.chain.from_iterable(
            compiled_files(cmds) for cmds in compile_commands
        )
    )
    return [f for f in files if f not in compiled and f.suffix in extensions]


def check_bazel_build_for_files(
    bazel_extensions_to_check: Container[str],
    files: Iterable[Path],
    bazel_dirs: Iterable[Path] = (),
) -> list[Path]:
    """Checks that source files are in the Bazel builds.

    Args:
        bazel_extensions_to_check: which file suffixes to look for in Bazel
        files: the files that should be checked
        bazel_dirs: directories in which to run bazel query

    Returns:
        a list of missing files; will be empty if there were no missing files
    """

    # Collect all paths in the Bazel builds.
    bazel_builds: Set[Path] = set()
    for directory in bazel_dirs:
        bazel_builds.update(
            _get_paths_from_command(
                directory,
                BAZEL_EXECUTABLE,
                'query',
                'kind("source file", //...:*)',
            )
        )

    missing: list[Path] = []

    if bazel_dirs:
        for path in (p for p in files if p.suffix in bazel_extensions_to_check):
            if path not in bazel_builds:
                # TODO: b/234883555 - Replace this workaround for fuzzers.
                if 'fuzz' not in str(path):
                    missing.append(path)

    if missing:
        _LOG.warning(
            '%s missing from the Bazel build:\n%s',
            plural(missing, 'file', are=True),
            '\n'.join(str(x) for x in missing),
        )

    return missing


def check_gn_build_for_files(
    gn_extensions_to_check: Container[str],
    files: Iterable[Path],
    gn_dirs: Iterable[tuple[Path, Path]] = (),
    gn_build_files: Iterable[Path] = (),
) -> list[Path]:
    """Checks that source files are in the GN build.

    Args:
        gn_extensions_to_check: which file suffixes to look for in GN
        files: the files that should be checked
        gn_dirs: (source_dir, output_dir) tuples with which to run gn desc
        gn_build_files: paths to BUILD.gn files to directly search for paths

    Returns:
        a list of missing files; will be empty if there were no missing files
    """

    # Collect all paths in GN builds.
    gn_builds: Set[Path] = set()

    for source_dir, output_dir in gn_dirs:
        gn_builds.update(
            _get_paths_from_command(source_dir, 'gn', 'desc', output_dir, '*')
        )

    gn_builds.update(_search_files_for_paths(gn_build_files))

    missing: list[Path] = []

    if gn_dirs or gn_build_files:
        for path in (p for p in files if p.suffix in gn_extensions_to_check):
            if path not in gn_builds:
                missing.append(path)

    if missing:
        _LOG.warning(
            '%s missing from the GN build:\n%s',
            plural(missing, 'file', are=True),
            '\n'.join(str(x) for x in missing),
        )

    return missing


def check_soong_build_for_files(
    soong_extensions_to_check: Container[str],
    files: Iterable[Path],
    soong_build_files: Iterable[Path] = (),
) -> list[Path]:
    """Checks that source files are in the Soong build.

    Args:
        bp_extensions_to_check: which file suffixes to look for in Soong files
        files: the files that should be checked
        bp_build_files: paths to Android.bp files to directly search for paths

    Returns:
        a list of missing files; will be empty if there were no missing files
    """

    # Collect all paths in Soong builds.
    soong_builds = set(_search_files_for_paths(soong_build_files))

    missing: list[Path] = []

    if soong_build_files:
        for path in (p for p in files if p.suffix in soong_extensions_to_check):
            if path not in soong_builds:
                missing.append(path)

    if missing:
        _LOG.warning(
            '%s missing from the Soong build:\n%s',
            plural(missing, 'file', are=True),
            '\n'.join(str(x) for x in missing),
        )

    return missing


def check_builds_for_files(
    bazel_extensions_to_check: Container[str],
    gn_extensions_to_check: Container[str],
    files: Iterable[Path],
    bazel_dirs: Iterable[Path] = (),
    gn_dirs: Iterable[tuple[Path, Path]] = (),
    gn_build_files: Iterable[Path] = (),
) -> dict[str, list[Path]]:
    """Checks that source files are in the GN and Bazel builds.

    Args:
        bazel_extensions_to_check: which file suffixes to look for in Bazel
        gn_extensions_to_check: which file suffixes to look for in GN
        files: the files that should be checked
        bazel_dirs: directories in which to run bazel query
        gn_dirs: (source_dir, output_dir) tuples with which to run gn desc
        gn_build_files: paths to BUILD.gn files to directly search for paths

    Returns:
        a dictionary mapping build system ('Bazel' or 'GN' to a list of missing
        files; will be empty if there were no missing files
    """

    bazel_missing = check_bazel_build_for_files(
        bazel_extensions_to_check=bazel_extensions_to_check,
        files=files,
        bazel_dirs=bazel_dirs,
    )
    gn_missing = check_gn_build_for_files(
        gn_extensions_to_check=gn_extensions_to_check,
        files=files,
        gn_dirs=gn_dirs,
        gn_build_files=gn_build_files,
    )

    result = {}
    if bazel_missing:
        result['Bazel'] = bazel_missing
    if gn_missing:
        result['GN'] = gn_missing
    return result


@contextlib.contextmanager
def test_server(executable: str, output_dir: Path):
    """Context manager that runs a test server executable.

    Args:
        executable: name of the test server executable
        output_dir: path to the output directory (for logs)
    """

    with open(output_dir / 'test_server.log', 'w') as outs:
        try:
            proc = subprocess.Popen(
                [executable, '--verbose'],
                stdout=outs,
                stderr=subprocess.STDOUT,
            )

            yield

        finally:
            proc.terminate()  # pylint: disable=used-before-assignment


@contextlib.contextmanager
def modified_env(**envvars):
    """Context manager that sets environment variables.

    Use by assigning values to variable names in the argument list, e.g.:
        `modified_env(MY_FLAG="some value")`

    Args:
        envvars: Keyword arguments
    """
    saved_env = os.environ.copy()
    os.environ.update(envvars)
    try:
        yield
    finally:
        os.environ = saved_env


def fuzztest_prng_seed(ctx: PresubmitContext) -> str:
    """Convert the RNG seed to the format expected by FuzzTest.

    FuzzTest can be configured to use the seed by setting the
    `FUZZTEST_PRNG_SEED` environment variable to this value.

    Args:
        ctx: The context that includes a pseudorandom number generator seed.
    """
    rng_bytes = ctx.rng_seed.to_bytes(32, sys.byteorder)
    return base64.urlsafe_b64encode(rng_bytes).decode('ascii').rstrip('=')


@filter_paths(
    file_filter=FileFilter(
        endswith=('.bzl', '.bazel'),
        name=('WORKSPACE',),
        exclude=(r'pw_presubmit/py/pw_presubmit/format/test_data',),
    )
)
def bazel_lint(ctx: PresubmitContext):
    """Runs buildifier with lint on Bazel files.

    Should be run after bazel_format since that will give more useful output
    for formatting-only issues.
    """

    failure = False
    for path in ctx.paths:
        try:
            call('buildifier', '--lint=warn', '--mode=check', path)
        except PresubmitFailure:
            failure = True

    if failure:
        raise PresubmitFailure


@Check
def gn_gen_check(ctx: PresubmitContext):
    """Runs gn gen --check to enforce correct header dependencies."""
    gn_gen(ctx, gn_check=True)
