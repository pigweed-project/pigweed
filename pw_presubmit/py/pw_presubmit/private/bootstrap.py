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
"""Steps for bootstrapping and build systems relying on bootstrap."""

import logging
import os
import shlex
import subprocess

from pw_presubmit.format.cpp import CPP_EXTS
from pw_presubmit.private import tools
from pw_presubmit.v2 import Context, PresubmitFailure, step

_LOG = logging.getLogger('pw_presubmit')


def _clean_env() -> dict[str, str]:
    """Returns environment without Bazel-specific directory variables."""
    env = os.environ.copy()
    env.pop('BUILD_WORKSPACE_DIRECTORY', None)
    env.pop('BUILD_WORKING_DIRECTORY', None)
    return env


def _run_in_bootstrapped_env(
    ctx: Context, cmd: str
) -> subprocess.CompletedProcess:
    """Runs a shell command in the activated bootstrap environment."""
    return tools.run_subprocess(
        f'. ./activate.sh && {cmd}',
        cwd=ctx.root,
        env=_clean_env(),
        shell=True,
    )


@step()
def bootstrap(ctx: Context) -> None:
    """Runs bootstrap.sh to set up the environment."""
    _LOG.info('Running bootstrap.sh')
    res = tools.run_subprocess(
        ['pw_env_setup/run.sh', 'bootstrap.sh'],
        cwd=ctx.root,
        env=_clean_env(),
    )
    if res.returncode != 0:
        raise PresubmitFailure('bootstrap failed')


@step(
    suffix=CPP_EXTS,
    endswith=('.cmake',),
    match_name=('CMakeLists.txt',),
)
def cmake_tests(ctx: Context) -> None:
    """Runs all CMake tests."""
    if not ctx.paths:
        return

    # Use host_clang toolchain as specified in cmake_build.sh
    toolchain_path = (
        ctx.root / 'pw_toolchain' / 'host_clang' / 'toolchain.cmake'
    )

    _LOG.info('Configuring CMake in %s', ctx.output_dir)
    res = _run_in_bootstrapped_env(
        ctx,
        f'cmake -B {shlex.quote(str(ctx.output_dir))} '
        f'-S {shlex.quote(str(ctx.root))} -G Ninja '
        f'-DCMAKE_TOOLCHAIN_FILE={shlex.quote(str(toolchain_path))}',
    )
    if res.returncode != 0:
        raise PresubmitFailure('cmake configure failed')

    # Run Ninja to build pw_apps and run pw_run_tests.modules
    _LOG.info('Running Ninja in %s', ctx.output_dir)
    res = _run_in_bootstrapped_env(
        ctx,
        f'ninja -C {shlex.quote(str(ctx.output_dir))} '
        f'--quiet pw_apps pw_run_tests.modules',
    )
    if res.returncode != 0:
        raise PresubmitFailure('ninja build/test failed')


@step(
    suffix=(*CPP_EXTS, '.py', '.gn', '.gni'),
)
def gn_gen(ctx: Context) -> None:
    """Run gn gen --check out."""
    if not ctx.paths:
        return
    res = _run_in_bootstrapped_env(
        ctx,
        'gn gen --check out',
    )
    if res.returncode != 0:
        raise PresubmitFailure('gn gen failed')


@step(
    suffix=(*CPP_EXTS, '.gn', '.gni'),
)
def gn_ninja_build(ctx: Context) -> None:
    """Run ninja -C out host_clang_debug stm32f429i."""
    if not ctx.paths:
        return
    res = _run_in_bootstrapped_env(
        ctx,
        'ninja -C out host_clang_debug stm32f429i',
    )
    if res.returncode != 0:
        raise PresubmitFailure('ninja host_clang_debug stm32f429i failed')


@step(
    suffix=('.py', '.gn', '.gni'),
)
def gn_ninja_python(ctx: Context) -> None:
    """Run ninja -C out python.lint python.tests."""
    if not ctx.paths:
        return
    res = _run_in_bootstrapped_env(
        ctx,
        'ninja -C out python.lint python.tests',
    )
    if res.returncode != 0:
        raise PresubmitFailure('ninja python.lint python.tests failed')
