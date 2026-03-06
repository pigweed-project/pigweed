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
from dataclasses import dataclass
import io
import itertools
import json
import logging
import os
import posixpath
from pathlib import Path
import re
import subprocess
import sys
import tarfile
from typing import (
    Any,
    Callable,
    Collection,
    Container,
    ContextManager,
    Iterable,
    Iterator,
    Mapping,
    Sequence,
    Set,
)

from pw_build.build_recipe import gn_args
import pw_cli.color
from pw_cli.plural import plural
from pw_cli.file_filter import FileFilter
from pw_presubmit.presubmit import (
    call,
    Check,
    filter_paths,
    install_package,
    PresubmitResult,
    SubStep,
)
from pw_presubmit.presubmit_context import (
    LuciContext,
    LuciTrigger,
    PresubmitContext,
    PresubmitFailure,
)
from pw_presubmit import (
    bazel_parser,
    format_code,
    ninja_parser,
)
from pw_presubmit.tools import (
    log_run,
    format_command,
)

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
                with ctx.failure_summary_log.open('w') as outs:
                    outs.write(failure)

        raise exc


def gn_gen(
    ctx: PresubmitContext,
    *args: str,
    gn_check: bool = True,  # pylint: disable=redefined-outer-name
    gn_fail_on_unused: bool = True,
    export_compile_commands: bool | str = True,
    preserve_args_gn: bool = False,
    **gn_arguments,
) -> None:
    """Runs gn gen in the specified directory with optional GN args.

    Runs with --check=system if gn_check=True. Note that this does not cover
    generated files. Run gn_check() after building to check generated files.
    """
    all_gn_args = {'pw_build_COLORIZE_OUTPUT': pw_cli.color.is_enabled()}
    all_gn_args.update(gn_arguments)
    all_gn_args.update(ctx.override_gn_args)
    _LOG.debug('%r', all_gn_args)
    args_option = gn_args(**all_gn_args)

    if not ctx.dry_run and not preserve_args_gn:
        # Delete args.gn to ensure this is a clean build.
        args_gn = ctx.output_dir / 'args.gn'
        if args_gn.is_file():
            args_gn.unlink()

    export_commands_arg = ''
    if export_compile_commands:
        export_commands_arg = '--export-compile-commands'
        if isinstance(export_compile_commands, str):
            export_commands_arg += f'={export_compile_commands}'

    call(
        'gn',
        '--color' if pw_cli.color.is_enabled() else '--nocolor',
        'gen',
        ctx.output_dir,
        *(['--check=system'] if gn_check else []),
        *(['--fail-on-unused-args'] if gn_fail_on_unused else []),
        *([export_commands_arg] if export_commands_arg else []),
        *args,
        *([args_option] if all_gn_args else []),
        cwd=ctx.root,
        call_annotation={
            'gn_gen_args': all_gn_args,
            'gn_gen_args_option': args_option,
        },
    )


def gn_check(ctx: PresubmitContext) -> PresubmitResult:
    """Runs gn check, including on generated and system files."""
    call(
        'gn',
        'check',
        ctx.output_dir,
        '--check-generated',
        '--check-system',
        cwd=ctx.root,
    )
    return PresubmitResult.PASS


def ninja(
    ctx: PresubmitContext,
    *args,
    save_compdb: bool = True,
    save_graph: bool = True,
    **kwargs,
) -> None:
    """Runs ninja in the specified directory."""

    num_jobs: list[str] = []
    if ctx.num_jobs is not None:
        num_jobs.extend(('-j', str(ctx.num_jobs)))

    keep_going: list[str] = []
    if ctx.continue_after_build_error:
        keep_going.extend(('-k', '0'))

    if save_compdb:
        proc = log_run(
            ['ninja', '-C', ctx.output_dir, '-t', 'compdb', *args],
            capture_output=True,
            **kwargs,
        )
        if not ctx.dry_run:
            (ctx.output_dir / 'ninja.compdb').write_bytes(proc.stdout)

    if save_graph:
        proc = log_run(
            ['ninja', '-C', ctx.output_dir, '-t', 'graph', *args],
            capture_output=True,
            **kwargs,
        )
        if not ctx.dry_run:
            (ctx.output_dir / 'ninja.graph').write_bytes(proc.stdout)

    ninja_stdout = ctx.output_dir / 'ninja.stdout'
    ctx.output_dir.mkdir(exist_ok=True, parents=True)
    try:
        with ninja_stdout.open('w') as outs:
            if sys.platform == 'win32':
                # Windows doesn't support pw-wrap-ninja.
                ninja_command = ['ninja']
            else:
                ninja_command = ['pw-wrap-ninja', '--log-actions']

            call(
                *ninja_command,
                '-C',
                ctx.output_dir,
                *num_jobs,
                *keep_going,
                *args,
                tee=outs,
                propagate_sigterm=True,
                call_annotation={'build_system': 'ninja'},
                **kwargs,
            )

    except PresubmitFailure as exc:
        failure = ninja_parser.parse_ninja_stdout(ninja_stdout)
        if failure:
            with ctx.failure_summary_log.open('w') as outs:
                outs.write(failure)

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


Item = int | str
Value = Item | Sequence[Item]
ValueCallable = Callable[[PresubmitContext], Value]
InputItem = Item | ValueCallable
InputValue = InputItem | Sequence[InputItem]


def _value(ctx: PresubmitContext, val: InputValue) -> Value:
    """Process any lambdas inside val

    val is a single value or a list of values, any of which might be a lambda
    that needs to be resolved. Call each of these lambdas with ctx and replace
    the lambda with the result. Return the updated top-level structure.
    """

    if isinstance(val, (str, int)):
        return val
    if callable(val):
        return val(ctx)

    result: list[Item] = []
    for item in val:
        if callable(item):
            call_result = item(ctx)
            if isinstance(call_result, (int, str)):
                result.append(call_result)
            else:  # Sequence.
                result.extend(call_result)
        elif isinstance(item, (int, str)):
            result.append(item)
        else:  # Sequence.
            result.extend(item)
    return result


_CtxMgrLambda = Callable[[PresubmitContext], ContextManager]
_CtxMgrOrLambda = ContextManager | _CtxMgrLambda


@dataclass(frozen=True)
class CommonCoverageOptions:
    """Coverage options shared by both CodeSearch and Gerrit.

    For Google use only.
    """

    # The "root" of the Kalypsi GCS bucket path to which the coverage data
    # should be uploaded. Typically gs://ng3-metrics/ng3-<teamname>-coverage.
    target_bucket_root: str

    # The project name in the Kalypsi GCS bucket path.
    target_bucket_project: str

    # See go/kalypsi-abs#trace-type-required.
    trace_type: str

    # go/kalypsi-abs#owner-required.
    owner: str

    # go/kalypsi-abs#bug-component-required.
    bug_component: str


@dataclass(frozen=True)
class CodeSearchCoverageOptions:
    """CodeSearch-specific coverage options. For Google use only."""

    # The name of the Gerrit host containing the CodeSearch repo. Just the name
    # ("pigweed"), not the full URL ("pigweed.googlesource.com"). This may be
    # different from the host from which the code was originally checked out.
    host: str

    # The name of the project, as expected by CodeSearch. Typically
    # 'codesearch'.
    project: str

    # See go/kalypsi-abs#ref-required.
    ref: str

    # See go/kalypsi-abs#source-required.
    source: str

    # See go/kalypsi-abs#add-prefix-optional.
    add_prefix: str = ''


@dataclass(frozen=True)
class GerritCoverageOptions:
    """Gerrit-specific coverage options. For Google use only."""

    # The name of the project, as expected by Gerrit. This is typically the
    # repository name, e.g. 'pigweed/pigweed' for upstream Pigweed.
    # See go/kalypsi-inc#project-required.
    project: str


@dataclass(frozen=True)
class CoverageOptions:
    """Coverage collection configuration. For Google use only."""

    common: CommonCoverageOptions
    codesearch: tuple[CodeSearchCoverageOptions, ...]
    gerrit: GerritCoverageOptions


class _NinjaBase(Check):
    """Thin wrapper of Check for steps that call ninja."""

    def __init__(
        self,
        *args,
        packages: Sequence[str] = (),
        ninja_contexts: Sequence[_CtxMgrOrLambda] = (),
        ninja_targets: str | Sequence[str] | Sequence[Sequence[str]] = (),
        coverage_options: CoverageOptions | None = None,
        **kwargs,
    ):
        """Initializes a _NinjaBase object.

        Args:
            *args: Passed on to superclass.
            packages: List of 'pw package' packages to install.
            ninja_contexts: List of context managers to apply around ninja
                calls.
            ninja_targets: Single ninja target, list of Ninja targets, or list
                of list of ninja targets. If a list of a list, ninja will be
                called multiple times with the same build directory.
            coverage_options: Coverage collection options (or None, if not
                collecting coverage data).
            **kwargs: Passed on to superclass.
        """
        super().__init__(*args, **kwargs)
        self._packages: Sequence[str] = packages
        self._ninja_contexts: tuple[_CtxMgrOrLambda, ...] = tuple(
            ninja_contexts
        )
        self._coverage_options = coverage_options

        if isinstance(ninja_targets, str):
            ninja_targets = (ninja_targets,)
        ninja_targets = list(ninja_targets)
        all_strings = all(isinstance(x, str) for x in ninja_targets)
        any_strings = any(isinstance(x, str) for x in ninja_targets)
        if ninja_targets and all_strings != any_strings:
            raise ValueError(repr(ninja_targets))

        self._ninja_target_lists: tuple[tuple[str, ...], ...]
        if all_strings:
            targets: list[str] = []
            for target in ninja_targets:
                targets.append(target)  # type: ignore
            self._ninja_target_lists = (tuple(targets),)
        else:
            self._ninja_target_lists = tuple(tuple(x) for x in ninja_targets)

    @property
    def ninja_targets(self) -> list[str]:
        return list(itertools.chain(*self._ninja_target_lists))

    def _install_package(  # pylint: disable=no-self-use
        self,
        ctx: PresubmitContext,
        package: str,
    ) -> PresubmitResult:
        install_package(ctx, package)
        return PresubmitResult.PASS

    @contextlib.contextmanager
    def _context(self, ctx: PresubmitContext):
        """Apply any context managers necessary for building."""
        with contextlib.ExitStack() as stack:
            for mgr in self._ninja_contexts:
                if isinstance(mgr, contextlib.AbstractContextManager):
                    stack.enter_context(mgr)
                else:
                    stack.enter_context(mgr(ctx))  # type: ignore
            yield

    def _ninja(
        self, ctx: PresubmitContext, targets: Sequence[str]
    ) -> PresubmitResult:
        with self._context(ctx):
            ninja(ctx, *targets)
        return PresubmitResult.PASS

    def _coverage(
        self, ctx: PresubmitContext, options: CoverageOptions
    ) -> PresubmitResult:
        """Archive and (on LUCI) upload coverage reports."""
        reports = ctx.output_dir / 'coverage_reports'
        os.makedirs(reports, exist_ok=True)
        coverage_jsons: list[Path] = []
        for path in ctx.output_dir.rglob('coverage_report'):
            _LOG.debug('exploring %s', path)
            name = str(path.relative_to(ctx.output_dir))
            name = name.replace('_', '').replace('/', '_')
            with tarfile.open(reports / f'{name}.tar.gz', 'w:gz') as tar:
                tar.add(path, arcname=name, recursive=True)
            json_path = path / 'json' / 'report.json'
            if json_path.is_file():
                _LOG.debug('found json %s', json_path)
                coverage_jsons.append(json_path)

        if not coverage_jsons:
            ctx.fail('No coverage json file found')
            return PresubmitResult.FAIL

        if len(coverage_jsons) > 1:
            _LOG.warning(
                'More than one coverage json file, selecting first: %r',
                coverage_jsons,
            )

        coverage_json = coverage_jsons[0]

        if ctx.luci:
            if not ctx.luci.is_prod:
                _LOG.warning('Not uploading coverage since not running in prod')
                return PresubmitResult.PASS

            with self._context(ctx):
                metadata_json_paths = _write_coverage_metadata(ctx, options)
                for i, metadata_json in enumerate(metadata_json_paths):
                    # GCS bucket paths are POSIX-like.
                    coverage_gcs_path = posixpath.join(
                        options.common.target_bucket_root,
                        'incremental' if ctx.luci.is_try else 'absolute',
                        options.common.target_bucket_project,
                        f'{ctx.luci.buildbucket_id}-{i}',
                    )
                    _copy_to_gcs(
                        ctx,
                        coverage_json,
                        posixpath.join(coverage_gcs_path, 'report.json'),
                    )
                    _copy_to_gcs(
                        ctx,
                        metadata_json,
                        posixpath.join(coverage_gcs_path, 'metadata.json'),
                    )

                return PresubmitResult.PASS

        _LOG.warning('Not uploading coverage since running locally')
        return PresubmitResult.PASS

    def _package_substeps(self) -> Iterator[SubStep]:
        for package in self._packages:
            yield SubStep(
                f'install {package} package',
                self._install_package,
                (package,),
            )

    def _ninja_substeps(self) -> Iterator[SubStep]:
        targets_parts = set()
        for targets in self._ninja_target_lists:
            targets_part = " ".join(targets)
            maxlen = 70
            if len(targets_part) > maxlen:
                targets_part = f'{targets_part[0:maxlen-3]}...'
            assert targets_part not in targets_parts
            targets_parts.add(targets_part)
            yield SubStep(f'ninja {targets_part}', self._ninja, (targets,))

    def _coverage_substeps(self) -> Iterator[SubStep]:
        if self._coverage_options is not None:
            yield SubStep('coverage', self._coverage, (self._coverage_options,))


def _copy_to_gcs(ctx: PresubmitContext, filepath: Path, gcs_dst: str):
    luci = Path(pw_cli.env.pigweed_environment().PW_LUCI_CIPD_INSTALL_DIR)
    gsutil = luci / 'gsutil' / 'gsutil'

    cmd = ['gcloud', 'storage', 'cp', filepath, gcs_dst]

    upload_stdout = ctx.output_dir / (filepath.name + '.stdout')
    with upload_stdout.open('w') as outs:
        call(*cmd, tee=outs)


class NoPrimaryTriggerError(Exception):
    pass


def _get_primary_change(ctx: PresubmitContext) -> LuciTrigger:
    assert ctx.luci is not None

    if len(ctx.luci.triggers) == 1:
        return ctx.luci.triggers[0]

    for trigger in ctx.luci.triggers:
        if trigger.primary:
            return trigger

    raise NoPrimaryTriggerError(repr(ctx.luci.triggers))


def _write_coverage_metadata(
    ctx: PresubmitContext, options: CoverageOptions
) -> Sequence[Path]:
    """Write out Kalypsi coverage metadata file(s) and return their paths."""
    assert ctx.luci is not None
    change = _get_primary_change(ctx)

    metadata = {
        'trace_type': options.common.trace_type,
        'trim_prefix': str(ctx.root),
        'patchset_num': change.patchset,
        'change_id': change.number,
        'owner': options.common.owner,
        'bug_component': options.common.bug_component,
    }

    if ctx.luci.is_try:
        # Running in CQ: uploading incremental coverage
        metadata.update(
            {
                'change_id': change.number,
                'host': change.gerrit_name,
                'patchset_num': change.patchset,
                'project': options.gerrit.project,
            }
        )

        metadata_json = ctx.output_dir / "metadata.json"
        with metadata_json.open('w') as metadata_file:
            json.dump(metadata, metadata_file)
        return (metadata_json,)

    # Running in CI: uploading absolute coverage, possibly to multiple locations
    # since a repo could be in codesearch in multiple places.
    metadata_jsons = []
    for i, cs in enumerate(options.codesearch):
        metadata.update(
            {
                'add_prefix': cs.add_prefix,
                'commit_id': change.ref,
                'host': cs.host,
                'project': cs.project,
                'ref': cs.ref,
                'source': cs.source,
            }
        )

        metadata_json = ctx.output_dir / f'metadata-{i}.json'
        with metadata_json.open('w') as metadata_file:
            json.dump(metadata, metadata_file)
        metadata_jsons.append(metadata_json)

    return tuple(metadata_jsons)


class GnGenNinja(_NinjaBase):
    """Thin wrapper of Check for steps that just call gn/ninja.

    Runs gn gen, ninja, then gn check.
    """

    def __init__(
        self,
        *args,
        gn_args: (  # pylint: disable=redefined-outer-name
            dict[str, Any] | None
        ) = None,
        **kwargs,
    ):
        """Initializes a GnGenNinja object.

        Args:
            *args: Passed on to superclass.
            gn_args: dict of GN args.
            **kwargs: Passed on to superclass.
        """
        super().__init__(self._substeps(), *args, **kwargs)
        self._gn_args: dict[str, Any] = gn_args or {}

    def add_default_gn_args(self, args):
        """Add any project-specific default GN args to 'args'."""

    @property
    def gn_args(self) -> dict[str, Any]:
        return self._gn_args

    def _gn_gen(self, ctx: PresubmitContext) -> PresubmitResult:
        args: dict[str, Any] = {}
        if self._coverage_options is not None:
            args['pw_toolchain_COVERAGE_ENABLED'] = True
            args['pw_build_PYTHON_TEST_COVERAGE'] = True

            if ctx.incremental:
                args['pw_toolchain_PROFILE_SOURCE_FILES'] = [
                    f'//{x.relative_to(ctx.root)}' for x in ctx.paths
                ]

        self.add_default_gn_args(args)

        args.update({k: _value(ctx, v) for k, v in self._gn_args.items()})
        gn_gen(ctx, gn_check=False, **args)  # type: ignore
        return PresubmitResult.PASS

    def _substeps(self) -> Iterator[SubStep]:
        yield from self._package_substeps()

        yield SubStep('gn gen', self._gn_gen)

        yield from self._ninja_substeps()

        # Run gn check after building so it can check generated files.
        yield SubStep('gn check', gn_check)

        yield from self._coverage_substeps()
