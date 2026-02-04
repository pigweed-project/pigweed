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
"""Runs the presubmit checks for the Pigweed repository."""

import argparse
import json
import logging
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Pattern, Sequence

import pw_package.pigweed_packages
from pw_cli.file_filter import FileFilter

from pw_presubmit import (
    bazel_checks,
    build,
    cli,
    cpp_checks,
    format_code,
    gitmodules,
    javascript_checks,
    module_owners,
    npm_presubmit,
    pigweed_local_presubmit,
    pw_internal_namespace,
    python_checks,
    shell_checks,
    source_in_build,
    upstream_checks,
)
from pw_presubmit.install_hook import install_git_hook
from pw_presubmit.presubmit import Programs, call, filter_paths
from pw_presubmit.presubmit_context import PresubmitContext, PresubmitFailure
from pw_presubmit.tools import log_run

# Keep this import separate for now to avoid breaking downstream users.
# pylint: disable=unused-import
from pw_presubmit.upstream_checks import owners_lint_checks

# pylint: enable=unused-import

_LOG = logging.getLogger(__name__)

pw_package.pigweed_packages.initialize()

_OPTIMIZATION_LEVELS = 'debug', 'size_optimized', 'speed_optimized'


def _at_all_optimization_levels(target):
    for level in _OPTIMIZATION_LEVELS:
        yield f'{target}_{level}'


class PigweedGnGenNinja(build.GnGenNinja):
    """Add Pigweed-specific defaults to GnGenNinja."""

    def add_default_gn_args(self, args):
        """Add project-specific default GN args to 'args'."""
        args['pw_C_OPTIMIZATION_LEVELS'] = ('debug',)


def build_bazel(*args, **kwargs) -> None:
    build.bazel(
        *args, use_remote_cache=True, strict_module_lockfile=True, **kwargs
    )


#
# Build presubmit checks
#
gn_all = PigweedGnGenNinja(
    name='gn_all',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    gn_args=dict(pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS),
    ninja_targets=('all',),
)


def gn_clang_build(ctx: PresubmitContext):
    """Checks all compile targets that rely on LLVM tooling."""
    build_targets = [
        *_at_all_optimization_levels('host_clang'),
        'cpp20_compatibility',
        'asan',
        'tsan',
        'ubsan',
        'runtime_sanitizers',
        # TODO: b/234876100 - msan will not work until the C++ standard library
        # included in the sysroot has a variant built with msan.
    ]

    # clang-tidy doesn't run on Windows.
    if sys.platform != 'win32':
        build_targets.append('static_analysis')

    # QEMU doesn't run on Windows.
    if sys.platform != 'win32':
        # TODO: b/244604080 - For the pw::InlineString tests, qemu_clang_debug
        #     and qemu_clang_speed_optimized produce a binary too large for the
        #     QEMU target's 256KB flash. Restore debug and speed optimized
        #     builds when this is fixed.
        build_targets.append('qemu_clang_size_optimized')

    # TODO: b/240982565 - SocketStream currently requires Linux.
    if sys.platform.startswith('linux'):
        build_targets.append('integration_tests')

    build.gn_gen(ctx, pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS)
    build.ninja(ctx, *build_targets)
    build.gn_check(ctx)


_HOST_COMPILER = 'gcc' if sys.platform == 'win32' else 'clang'


@filter_paths(file_filter=upstream_checks.BUILD_FILE_FILTER)
def gn_quick_build_check(ctx: PresubmitContext):
    """Checks the state of the GN build by running gn gen and gn check."""
    build.gn_gen(ctx)


def _gn_main_build_check_targets() -> Sequence[str]:
    build_targets = [
        'check_modules',
        *_at_all_optimization_levels('stm32f429i'),
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        'python.tests',
        'python.lint',
        'pigweed_pypi_distribution',
    ]

    # Since there is no mac-arm64 bloaty binary in CIPD, Arm Macs use the x86_64
    # binary. However, Arm Macs in Pigweed CI disable Rosetta 2, so skip the
    # 'default' build on those machines for now.
    #
    # TODO: b/368387791 - Add 'default' for all platforms when Arm Mac bloaty is
    # available.
    if platform.machine() != 'arm64' or sys.platform != 'darwin':
        build_targets.append('default')

    return build_targets


def _gn_platform_build_check_targets() -> Sequence[str]:
    build_targets = []

    # TODO: b/315998985 - Add docs back to Mac ARM build.
    if sys.platform != 'darwin' or platform.machine() != 'arm64':
        build_targets.append('docs')

    # TODO: b/234645359 - Re-enable on Windows when compatibility tests build.
    if sys.platform != 'win32':
        build_targets.append('cpp20_compatibility')

    # clang-tidy doesn't run on Windows.
    if sys.platform != 'win32':
        build_targets.append('static_analysis')

    # QEMU doesn't run on Windows.
    if sys.platform != 'win32':
        # TODO: b/244604080 - For the pw::InlineString tests, qemu_*_debug
        #     and qemu_*_speed_optimized produce a binary too large for the
        #     QEMU target's 256KB flash. Restore debug and speed optimized
        #     builds when this is fixed.
        build_targets.append('qemu_gcc_size_optimized')
        build_targets.append('qemu_clang_size_optimized')

    # TODO: b/240982565 - SocketStream currently requires Linux.
    if sys.platform.startswith('linux'):
        build_targets.append('integration_tests')

    # TODO: b/269354373 - clang is not supported on windows yet
    if sys.platform != 'win32':
        build_targets.append('host_clang_debug_dynamic_allocation')

    return build_targets


def _gn_combined_build_check_targets() -> Sequence[str]:
    return [
        *_gn_main_build_check_targets(),
        *_gn_platform_build_check_targets(),
    ]


gn_main_build_check = PigweedGnGenNinja(
    name='gn_main_build_check',
    doc='Run most host.',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    gn_args=dict(
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
        pw_BUILD_BROKEN_GROUPS=True,  # Enable to fully test the GN build
    ),
    ninja_targets=_gn_main_build_check_targets(),
)

gn_platform_build_check = PigweedGnGenNinja(
    name='gn_platform_build_check',
    doc='Run any host platform-specific tests.',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    gn_args=dict(
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
        pw_BUILD_BROKEN_GROUPS=True,  # Enable to fully test the GN build
    ),
    ninja_targets=_gn_platform_build_check_targets(),
)

gn_combined_build_check = PigweedGnGenNinja(
    name='gn_combined_build_check',
    doc='Run most host and device (QEMU) tests.',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('emboss',),
    gn_args=dict(
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
        pw_BUILD_BROKEN_GROUPS=True,  # Enable to fully test the GN build
    ),
    ninja_targets=_gn_combined_build_check_targets(),
)

coverage = PigweedGnGenNinja(
    name='coverage',
    doc='Run coverage for the host build.',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('boringssl', 'emboss', 'googletest'),
    ninja_targets=('coverage',),
    coverage_options=build.CoverageOptions(
        common=build.CommonCoverageOptions(
            target_bucket_project='pigweed',
            target_bucket_root='gs://ng3-metrics/ng3-pigweed-coverage',
            trace_type='LLVM',
            owner='pigweed-infra@google.com',
            bug_component='503634',
        ),
        codesearch=(
            build.CodeSearchCoverageOptions(
                host='pigweed-internal',
                project='codesearch',
                add_prefix='pigweed',
                ref='refs/heads/main',
                source='infra:main',
            ),
        ),
        gerrit=build.GerritCoverageOptions(
            project='pigweed/pigweed',
        ),
    ),
    gn_args=dict(
        dir_pw_third_party_boringssl=lambda ctx: '"{}"'.format(
            ctx.package_root / 'boringssl'
        ),
        dir_pw_third_party_emboss=lambda ctx: '"{}"'.format(
            ctx.package_root / 'emboss'
        ),
        dir_pw_third_party_googletest=lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        pw_unit_test_MAIN=lambda ctx: '"{}"'.format(
            ctx.root / 'third_party/googletest:gmock_main'
        ),
        pw_unit_test_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_unit_test:googletest'
        ),
        pw_function_CONFIG=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_function:enable_dynamic_allocation'
        ),
        pw_crypto_AES_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:aes_boringssl'
        ),
        pw_crypto_ECDH_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdh_boringssl'
        ),
        pw_bluetooth_sapphire_ENABLED=True,
    ),
)


@filter_paths(file_filter=upstream_checks.BUILD_FILE_FILTER)
def gn_arm_build(ctx: PresubmitContext):
    build.gn_gen(ctx, pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS)
    build.ninja(ctx, *_at_all_optimization_levels('stm32f429i'))
    build.gn_check(ctx)


stm32f429i = PigweedGnGenNinja(
    name='stm32f429i',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    gn_args={
        'pw_use_test_server': True,
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_contexts=(
        lambda ctx: build.test_server(
            'stm32f429i_disc1_test_server',
            ctx.output_dir,
        ),
    ),
    ninja_targets=_at_all_optimization_levels('stm32f429i'),
)

gn_crypto_mbedtls_build = PigweedGnGenNinja(
    name='gn_crypto_mbedtls_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('mbedtls',),
    gn_args={
        'dir_pw_third_party_mbedtls': lambda ctx: '"{}"'.format(
            ctx.package_root / 'mbedtls'
        ),
        'pw_crypto_SHA256_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:sha256_mbedtls_v3'
        ),
        'pw_crypto_ECDSA_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdsa_mbedtls_v3'
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=(
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        # TODO: b/240982565 - SocketStream currently requires Linux.
        *(('integration_tests',) if sys.platform.startswith('linux') else ()),
    ),
)

gn_teensy_build = PigweedGnGenNinja(
    name='gn_teensy_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('teensy',),
    gn_args={
        'pw_arduino_build_CORE_PATH': lambda ctx: '"{}"'.format(
            str(ctx.package_root)
        ),
        'pw_arduino_build_CORE_NAME': 'teensy',
        'pw_arduino_build_PACKAGE_NAME': 'avr/1.58.1',
        'pw_arduino_build_BOARD': 'teensy40',
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=_at_all_optimization_levels('arduino'),
)

gn_pico_build = PigweedGnGenNinja(
    name='gn_pico_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('pico_sdk', 'freertos', 'emboss'),
    gn_args={
        'dir_pw_third_party_emboss': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'emboss')
        ),
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'freertos')
        ),
        'PICO_SRC_DIR': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'pico_sdk')
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('pi_pico',),
)

gn_mimxrt595_build = PigweedGnGenNinja(
    name='gn_mimxrt595_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('mcuxpresso',),
    gn_args={
        'dir_pw_third_party_mcuxpresso': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'mcuxpresso')
        ),
        # pylint: disable=line-too-long
        'pw_third_party_mcuxpresso_CONFIG': '//targets/mimxrt595_evk:mcuxpresso_sdk_config',
        'pw_third_party_mcuxpresso_SDK': '//targets/mimxrt595_evk:mcuxpresso_sdk',
        # pylint: enable=line-too-long
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('mimxrt595'),
)

gn_mimxrt595_freertos_build = PigweedGnGenNinja(
    name='gn_mimxrt595_freertos_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('freertos', 'mcuxpresso'),
    gn_args={
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'freertos')
        ),
        'dir_pw_third_party_mcuxpresso': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'mcuxpresso')
        ),
        # pylint: disable=line-too-long
        'pw_third_party_mcuxpresso_CONFIG': '//targets/mimxrt595_evk_freertos:mcuxpresso_sdk_config',
        'pw_third_party_mcuxpresso_SDK': '//targets/mimxrt595_evk_freertos:mcuxpresso_sdk',
        # pylint: enable=line-too-long
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('mimxrt595_freertos'),
)

gn_software_update_build = PigweedGnGenNinja(
    name='gn_software_update_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('nanopb', 'protobuf', 'mbedtls'),
    gn_args={
        'dir_pw_third_party_protobuf': lambda ctx: '"{}"'.format(
            ctx.package_root / 'protobuf'
        ),
        'dir_pw_third_party_nanopb': lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        'dir_pw_third_party_mbedtls': lambda ctx: '"{}"'.format(
            ctx.package_root / 'mbedtls'
        ),
        'pw_crypto_SHA256_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:sha256_mbedtls_v3'
        ),
        'pw_crypto_ECDSA_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdsa_mbedtls_v3'
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=_at_all_optimization_levels('host_clang'),
)

gn_pw_system_demo_build = PigweedGnGenNinja(
    name='gn_pw_system_demo_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('freertos', 'nanopb', 'stm32cube_f4', 'pico_sdk'),
    gn_args={
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            ctx.package_root / 'freertos'
        ),
        'dir_pw_third_party_nanopb': lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        'dir_pw_third_party_stm32cube_f4': lambda ctx: '"{}"'.format(
            ctx.package_root / 'stm32cube_f4'
        ),
        'PICO_SRC_DIR': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'pico_sdk')
        ),
    },
    ninja_targets=('pw_system_demo',),
)

gn_chre_googletest_nanopb_sapphire_build = PigweedGnGenNinja(
    name='gn_chre_googletest_nanopb_sapphire_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('boringssl', 'chre', 'emboss', 'googletest', 'nanopb'),
    gn_args=dict(
        dir_pw_third_party_chre=lambda ctx: '"{}"'.format(
            ctx.package_root / 'chre'
        ),
        dir_pw_third_party_nanopb=lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        dir_pw_third_party_googletest=lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        dir_pw_third_party_emboss=lambda ctx: '"{}"'.format(
            ctx.package_root / 'emboss'
        ),
        dir_pw_third_party_boringssl=lambda ctx: '"{}"'.format(
            ctx.package_root / 'boringssl'
        ),
        pw_unit_test_MAIN=lambda ctx: '"{}"'.format(
            ctx.root / 'third_party/googletest:gmock_main'
        ),
        pw_unit_test_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_unit_test:googletest'
        ),
        pw_function_CONFIG=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_function:enable_dynamic_allocation'
        ),
        pw_crypto_AES_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:aes_boringssl'
        ),
        pw_crypto_ECDH_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdh_boringssl'
        ),
        pw_bluetooth_sapphire_ENABLED=True,
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
    ),
    ninja_targets=(
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        *_at_all_optimization_levels('stm32f429i'),
    ),
)

gn_fuzz_build = PigweedGnGenNinja(
    name='gn_fuzz_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('abseil-cpp', 'fuzztest', 'googletest'),
    gn_args={
        'dir_pw_third_party_abseil_cpp': lambda ctx: '"{}"'.format(
            ctx.package_root / 'abseil-cpp'
        ),
        'dir_pw_third_party_fuzztest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'fuzztest'
        ),
        'dir_pw_third_party_googletest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
    },
    ninja_targets=('fuzzers',),
    ninja_contexts=(
        lambda ctx: build.modified_env(
            FUZZTEST_PRNG_SEED=build.fuzztest_prng_seed(ctx),
        ),
    ),
)

oss_fuzz_build = PigweedGnGenNinja(
    name='oss_fuzz_build',
    path_filter=upstream_checks.BUILD_FILE_FILTER,
    packages=('abseil-cpp', 'fuzztest', 'googletest'),
    gn_args={
        'dir_pw_third_party_abseil_cpp': lambda ctx: '"{}"'.format(
            ctx.package_root / 'abseil-cpp'
        ),
        'dir_pw_third_party_fuzztest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'fuzztest'
        ),
        'dir_pw_third_party_googletest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        'pw_toolchain_OSS_FUZZ_ENABLED': True,
    },
    ninja_targets=('oss_fuzz',),
)


def _env_with_zephyr_vars(ctx: PresubmitContext) -> dict:
    """Returns the environment variables with ... set for Zephyr."""
    env = os.environ.copy()
    # Set some variables here.
    env['ZEPHYR_BASE'] = str(ctx.package_root / 'zephyr')
    env['ZEPHYR_MODULES'] = str(ctx.root)
    env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'llvm'
    return env


def zephyr_build(ctx: PresubmitContext) -> None:
    """Run Zephyr compatible tests"""
    # Install the Zephyr package
    build.install_package(ctx, 'zephyr')
    # Configure the environment
    env = _env_with_zephyr_vars(ctx)
    sysroot_dir = Path(
        os.environ['PW_PIGWEED_CIPD_INSTALL_DIR'], 'clang_sysroot'
    )
    platform_filters = (
        ['-P', 'native_sim']
        if platform.system() in ['Windows', 'Darwin']
        else []
    )
    # Run twister
    call(
        'pw',
        'twister-runner',
        '-vvv',
        '--ninja',
        '--integration',
        '--clobber-output',
        '--inline-logs',
        '--verbose',
        '--coverage',
        '--coverage-basedir',
        str(ctx.root),
        *platform_filters,
        f'-x=SYSROOT_DIR={sysroot_dir}',
        '--testsuite-root',
        str(ctx.root),
        env=env,
    )
    # Find all the raw profile files
    raw_profile_files = list(
        (ctx.root / 'twister-out').rglob('default.profraw')
    )
    # Find the corresponding executables
    executable_files = list((ctx.root / 'twister-out').rglob('zephyr.exe'))

    if not raw_profile_files or not executable_files:
        _LOG.info("No llvm coverage files generated, skipping coverage report")
        return

    # Needs to index the reports
    prof_data_file = ctx.root / 'twister-out' / 'coverage.profdata'
    call(
        *(
            [
                'llvm-profdata',
                'merge',
                '--sparse',
                '-o',
                str(prof_data_file),
            ]
            + [str(p) for p in raw_profile_files]
        )
    )

    # Produce the report in twister-out/coverage
    call(
        *(
            [
                'llvm-cov',
                'show',
                '--format=html',
                f'--instr-profile={prof_data_file}',
                f'--output-dir={ctx.root / "twister-out" / "coverage"}',
                '--ignore-filename-regex=(.*/)?(environment|twister-out)/.*',
            ]
            + [f'--object={p}' for p in executable_files]
        )
    )


def assert_non_empty_directory(directory: Path) -> None:
    if not directory.is_dir():
        raise PresubmitFailure(f'no directory {directory}')

    for _ in directory.iterdir():
        return

    raise PresubmitFailure(f'no files in {directory}')


def docs_build(ctx: PresubmitContext) -> None:
    """Build Pigweed docs"""
    if ctx.dry_run:
        raise PresubmitFailure(
            'This presubmit cannot be run in dry-run mode. '
            'Please run with: "pw presubmit --step"'
        )

    build.install_package(ctx, 'emboss')
    build.install_package(ctx, 'boringssl')
    build.install_package(ctx, 'freertos')
    build.install_package(ctx, 'nanopb')
    build.install_package(ctx, 'pico_sdk')
    build.install_package(ctx, 'pigweed_examples_repo')
    build.install_package(ctx, 'stm32cube_f4')
    emboss_dir = ctx.package_root / 'emboss'
    boringssl_dir = ctx.package_root / 'boringssl'
    pico_sdk_dir = ctx.package_root / 'pico_sdk'
    stm32cube_dir = ctx.package_root / 'stm32cube_f4'
    freertos_dir = ctx.package_root / 'freertos'
    nanopb_dir = ctx.package_root / 'nanopb'

    enable_dynamic_allocation = (
        ctx.root / 'pw_function:enable_dynamic_allocation'
    )

    # Build main docs through GN/Ninja.
    build.gn_gen(
        ctx,
        dir_pw_third_party_emboss=f'"{emboss_dir}"',
        dir_pw_third_party_boringssl=f'"{boringssl_dir}"',
        pw_bluetooth_sapphire_ENABLED=True,
        pw_function_CONFIG=f'"{enable_dynamic_allocation}"',
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
    )
    build.ninja(ctx, 'docs')
    build.gn_check(ctx)

    # Build Rust docs through Bazel.
    build_bazel(
        ctx,
        'build',
        '--remote_download_outputs=all',
        '--',
        '//pw_rust:docs',
    )

    # Build examples repo docs through GN.
    examples_repo_root = ctx.package_root / 'pigweed_examples_repo'
    examples_repo_out = examples_repo_root / 'out'

    # Setup an examples repo presubmit context.
    examples_ctx = PresubmitContext(
        root=examples_repo_root,
        repos=(examples_repo_root,),
        output_dir=examples_repo_out,
        failure_summary_log=ctx.failure_summary_log,
        paths=tuple(),
        all_paths=tuple(),
        package_root=ctx.package_root,
        luci=None,
        override_gn_args={},
        num_jobs=ctx.num_jobs,
        continue_after_build_error=True,
        _failed=False,
        format_options=ctx.format_options,
    )

    # Write a pigweed_environment.gni for the examples repo.
    pwenvgni = (
        ctx.root / 'build_overrides/pigweed_environment.gni'
    ).read_text()
    # Fix the path for cipd packages.
    pwenvgni.replace('../environment/cipd/', '../../cipd/')
    # Write the file
    (examples_repo_root / 'build_overrides/pigweed_environment.gni').write_text(
        pwenvgni
    )

    # Set required GN args.
    build.gn_gen(
        examples_ctx,
        dir_pigweed='"//../../.."',
        dir_pw_third_party_stm32cube_f4=f'"{stm32cube_dir}"',
        dir_pw_third_party_freertos=f'"{freertos_dir}"',
        dir_pw_third_party_nanopb=f'"{nanopb_dir}"',
        PICO_SRC_DIR=f'"{pico_sdk_dir}"',
    )
    build.ninja(examples_ctx, 'docs')

    # Copy rust docs from Bazel's out directory into where the GN build
    # put the main docs.
    rust_docs_bazel_dir = ctx.output_dir / 'bazel-bin/pw_rust/docs.rustdoc'
    rust_docs_output_dir = ctx.output_dir / 'docs/gen/docs/html/rustdoc'

    # Copy the doxygen html output to the main docs location.
    doxygen_html_gn_dir = ctx.output_dir / 'docs/doxygen/html'
    doxygen_html_output_dir = ctx.output_dir / 'docs/gen/docs/html/doxygen'

    # Copy the examples repo html output to the main docs location into
    # '/examples/'.
    examples_html_gn_dir = examples_repo_out / 'docs/gen/docs/html'
    examples_html_output_dir = ctx.output_dir / 'docs/gen/docs/html/examples'

    # Remove outputs to avoid including stale files from previous runs.
    shutil.rmtree(rust_docs_output_dir, ignore_errors=True)
    shutil.rmtree(doxygen_html_output_dir, ignore_errors=True)
    shutil.rmtree(examples_html_output_dir, ignore_errors=True)

    # Bazel generates files and directories without write permissions.  In
    # order to allow this rule to be run multiple times we use shutil.copyfile
    # for the actual copies to not copy permissions of files.
    shutil.copytree(
        rust_docs_bazel_dir,
        rust_docs_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )
    assert_non_empty_directory(rust_docs_output_dir)

    # Copy doxygen html outputs.
    shutil.copytree(
        doxygen_html_gn_dir,
        doxygen_html_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )
    assert_non_empty_directory(doxygen_html_output_dir)

    # mkdir -p the example repo output dir and copy the files over.
    examples_html_output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        examples_html_gn_dir,
        examples_html_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )
    assert_non_empty_directory(examples_html_output_dir)


def _run_cmake(ctx: PresubmitContext, toolchain='host_clang') -> None:
    build.install_package(ctx, 'emboss')
    build.install_package(ctx, 'flatbuffers')
    build.install_package(ctx, 'nanopb')

    env = None
    if 'clang' in toolchain:
        env = build.env_with_clang_vars()

    toolchain_path = ctx.root / 'pw_toolchain' / toolchain / 'toolchain.cmake'
    build.cmake(
        ctx,
        '--fresh',
        f'-DCMAKE_TOOLCHAIN_FILE={toolchain_path}',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=1',
        f'-Ddir_pw_third_party_emboss={ctx.package_root / "emboss"}',
        f'-Ddir_pw_third_party_flatbuffers={ctx.package_root / "flatbuffers"}',
        f'-Ddir_pw_third_party_nanopb={ctx.package_root / "nanopb"}',
        '-Dpw_third_party_nanopb_ADD_SUBDIRECTORY=ON',
        env=env,
    )


CMAKE_TARGETS = [
    'pw_apps',
    'pw_run_tests.modules',
]


@filter_paths(
    endswith=(*format_code.C_FORMAT.extensions, '.cmake', 'CMakeLists.txt')
)
def cmake_clang(ctx: PresubmitContext):
    _run_cmake(ctx, toolchain='host_clang')
    build.ninja(ctx, *CMAKE_TARGETS)
    build.gn_check(ctx)


@filter_paths(
    endswith=(*format_code.C_FORMAT.extensions, '.cmake', 'CMakeLists.txt')
)
def cmake_gcc(ctx: PresubmitContext):
    _run_cmake(ctx, toolchain='host_gcc')
    build.ninja(ctx, *CMAKE_TARGETS)
    build.gn_check(ctx)


def bthost_package(ctx: PresubmitContext) -> None:
    """Builds, tests, and prepares bt_host for upload."""
    # Test that `@fuchsia_sdk` isn't fetched when building non-fuchsia targets.
    # We specifically want to disallow this behavior because `@fuchsia_sdk` is
    # large and expensive to fetch.
    non_fuchsia_build_cmd = [
        'bazel',
        'build',
        # TODO: https://pwbug.dev/392092401 - Use `--override_module` instead of
        # `--override_repository` here once this dep is migrated to bzlmod.
        '--override_repository=fuchsia_sdk=/disallow/fuchsia_sdk/download/',
        '//pw_status/...',
    ]
    try:
        build_bazel(ctx, *non_fuchsia_build_cmd[1:])
    except PresubmitFailure as exc:
        failure_message = (
            "ERROR: Non-Fuchsia targets must be able to build without the "
            "Fuchsia SDK.\nRepro command: " + shlex.join(non_fuchsia_build_cmd)
        )
        with ctx.failure_summary_log.open('w') as outs:
            outs.write(failure_message)
        raise PresubmitFailure(failure_message) from exc

    target = '//pw_bluetooth_sapphire/fuchsia:infra'
    build_bazel(ctx, 'build', '--config=fuchsia', target)

    # Explicitly specify TEST_UNDECLARED_OUTPUTS_DIR_OVERRIDE as that will allow
    # `orchestrate`'s output (eg: ffx host + target logs, test stdout/stderr) to
    # be picked up by the `save_logs` recipe module.
    # We cannot rely on Bazel's native TEST_UNDECLARED_OUTPUTS_DIR functionality
    # since `zip` is not available in builders. See https://pwbug.dev/362990622.
    build_bazel(
        ctx,
        'run',
        '--config=fuchsia',
        f'{target}.test_all',
        env=dict(
            os.environ,
            TEST_UNDECLARED_OUTPUTS_DIR_OVERRIDE=ctx.output_dir,
        ),
    )

    stdout_path = ctx.output_dir / 'bazel.manifest.stdout'
    with open(stdout_path, 'w') as outs:
        build_bazel(
            ctx,
            'build',
            '--config=fuchsia',
            '--output_groups=builder_manifest',
            target,
            stdout=outs,
        )

    manifest_path: Path | None = None
    for line in stdout_path.read_text().splitlines():
        line = line.strip()
        if line.endswith('infrabuilder_manifest.json'):
            manifest_path = Path(line)
            break
    else:
        raise PresubmitFailure('no manifest found in output')

    _LOG.debug('manifest: %s', manifest_path)
    shutil.copyfile(manifest_path, ctx.output_dir / 'builder_manifest.json')


@filter_paths(
    endswith=(
        *format_code.C_FORMAT.extensions,
        '.bazel',
        '.bzl',
        '.py',
        '.rs',
        'BUILD',
    )
)
def bazel_build(ctx: PresubmitContext) -> None:
    """Runs Bazel build for each supported platform."""
    # Build everything with the default flags.
    build_bazel(
        ctx,
        'build',
        '--',
        '//...',
    )

    # Mapping from Bazel platforms to targets which should be built for those
    # platforms.
    targets_for_config = {
        "lm3s6965evb": [
            "//pw_rust/...",
        ],
        "microbit": [
            "//pw_rust/...",
        ],
    }

    for cxxversion in ('17', '20'):
        # Explicitly build for each supported C++ version.
        args = [ctx, 'build', f"--//pw_toolchain/cc:cxx_standard={cxxversion}"]
        args += ['--', '//...']
        build_bazel(*args)

        for config, targets in targets_for_config.items():
            build_bazel(
                ctx,
                'build',
                f'--config={config}',
                f"--//pw_toolchain/cc:cxx_standard={cxxversion}",
                *targets,
            )

    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_freertos',
        '--//pw_thread_freertos:config_override=//pw_build:test_module_config',
        '//pw_build:module_config_test',
    )

    for rp2xxx in ('rp2040', 'rp2350'):
        # Build upstream Pigweed for the rp2040 and rp2350.
        # First using the config.
        build_bazel(
            ctx,
            'build',
            f'--config={rp2xxx}',
            '//...',
            # Bazel will silently skip any incompatible targets in wildcard
            # builds; but we know that some end-to-end targets definitely should
            # remain compatible with this platform. So we list them explicitly.
            # (If an explicitly listed target is incompatible with the platform,
            # Bazel will return an error instead of skipping it.)
            '//pw_bloat:bloat_base',
            '//pw_status:status_test',
        )
        # Then using the transition.
        #
        # This ensures that the rp2040_binary rule transition includes all
        # required backends.
        build_bazel(
            ctx,
            'build',
            f'//pw_system:{rp2xxx}_system_example',
        )

    # Build upstream Pigweed for the Discovery board using STM32Cube.
    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_freertos',
        '//...',
        # Bazel will silently skip any incompatible targets in wildcard builds;
        # but we know that some end-to-end targets definitely should remain
        # compatible with this platform. So we list them explicitly. (If an
        # explicitly listed target is incompatible with the platform, Bazel
        # will return an error instead of skipping it.)
        '//pw_system:system_example',
    )

    # Build upstream Pigweed for the Discovery board using the baremetal
    # backends.
    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_baremetal',
        '//...',
    )

    # Build the fuzztest example.
    #
    # TODO: b/324652164 - This doesn't work on MacOS yet.
    if sys.platform != 'darwin':
        build_bazel(
            ctx,
            'build',
            '--config=fuzztest',
            '//pw_fuzzer/examples/fuzztest:metrics_fuzztest',
        )


def pw_transfer_integration_test(ctx: PresubmitContext) -> None:
    """Runs the pw_transfer cross-language integration test only.

    This test is not part of the regular bazel build because it's slow and
    intended to run in CI only.
    """
    build_bazel(
        ctx,
        'test',
        '//pw_transfer/integration_test:cross_language_small_test',
        '//pw_transfer/integration_test:cross_language_medium_read_test',
        '//pw_transfer/integration_test:cross_language_medium_write_test',
        '//pw_transfer/integration_test:cross_language_large_read_test',
        '//pw_transfer/integration_test:cross_language_large_write_test',
        '//pw_transfer/integration_test:multi_transfer_test',
        '//pw_transfer/integration_test:expected_errors_test',
        '//pw_transfer/integration_test:legacy_binaries_test',
        '--test_output=errors',
    )


#
# General presubmit checks
#


def _clang_system_include_paths(lang: str) -> list[str]:
    """Generate default system header paths.

    Returns the list of system include paths used by the host
    clang installation.
    """
    # Dump system include paths with preprocessor verbose.
    command = [
        'clang++',
        '-Xpreprocessor',
        '-v',
        '-x',
        f'{lang}',
        f'{os.devnull}',
        '-fsyntax-only',
    ]
    process = log_run(
        command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )

    # Parse the command output to retrieve system include paths.
    # The paths are listed one per line.
    output = process.stdout.decode(errors='backslashreplace')
    include_paths: list[str] = []
    for line in output.splitlines():
        path = line.strip()
        if os.path.exists(path):
            include_paths.append(f'-isystem{path}')

    return include_paths


def edit_compile_commands(
    in_path: Path, out_path: Path, func: Callable[[str, str, str], str]
) -> None:
    """Edit the selected compile command file.

    Calls the input callback on all triplets (file, directory, command) in
    the input compile commands database. The return value replaces the old
    compile command in the output database.
    """
    with open(in_path) as in_file:
        compile_commands = json.load(in_file)
        for item in compile_commands:
            item['command'] = func(
                item['file'], item['directory'], item['command']
            )
    with open(out_path, 'w') as out_file:
        json.dump(compile_commands, out_file, indent=2)


@filter_paths(endswith=format_code.CPP_SOURCE_EXTS)
def source_is_in_cmake_build_warn_only(ctx: PresubmitContext):
    """Checks that source files are in the CMake build."""

    _run_cmake(ctx)
    missing = SOURCE_FILES_FILTER_CMAKE_EXCLUDE.filter(
        build.check_compile_commands_for_files(
            ctx.output_dir / 'compile_commands.json',
            (f for f in ctx.paths if f.suffix in format_code.CPP_SOURCE_EXTS),
        )
    )
    if missing:
        _LOG.warning(
            'Files missing from CMake:\n%s',
            '\n'.join(str(f) for f in missing),
        )


def build_env_setup(ctx: PresubmitContext):
    if 'PW_CARGO_SETUP' not in os.environ:
        _LOG.warning('Skipping build_env_setup since PW_CARGO_SETUP is not set')
        return

    tmpl = ctx.root.joinpath('pw_env_setup', 'py', 'pyoxidizer.bzl.tmpl')
    out = ctx.output_dir.joinpath('pyoxidizer.bzl')

    with open(tmpl, 'r') as ins:
        cfg = ins.read().replace('${PW_ROOT}', str(ctx.root))
        with open(out, 'w') as outs:
            outs.write(cfg)

    call('pyoxidizer', 'build', cwd=ctx.output_dir)


@filter_paths(endswith=(*format_code.C_FORMAT.extensions, '.py'))
def static_analysis(ctx: PresubmitContext):
    """Runs all available static analysis tools."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'python.lint', 'static_analysis')
    build.gn_check(ctx)


SOURCE_FILES_FILTER_CMAKE_EXCLUDE = FileFilter(
    exclude=(
        # keep-sorted: start
        r'\bpw_bluetooth_sapphire/fuchsia',
        r'\bpw_kernel',
        # keep-sorted: end
    ),
)

#
# Presubmit check programs
#

OTHER_CHECKS = (
    # keep-sorted: start
    bazel_checks.lockfile_check,
    bthost_package,
    build.gn_gen_check,
    cmake_clang,
    cmake_gcc,
    coverage,
    # TODO: b/234876100 - Remove once msan is added to all_sanitizers().
    cpp_checks.msan,
    docs_build,
    gitmodules.create(gitmodules.Config(allow_submodules=False)),
    gn_all,
    gn_clang_build,
    gn_combined_build_check,
    gn_main_build_check,
    gn_platform_build_check,
    module_owners.presubmit_check(),
    npm_presubmit.npm_test,
    npm_presubmit.vscode_test,
    pw_internal_namespace.pw_internal_namespace,
    pw_transfer_integration_test,
    python_checks.diff_upstream_python_constraints,
    python_checks.update_upstream_python_constraints,
    python_checks.upload_pigweed_pypi_distribution,
    python_checks.vendor_python_wheels,
    python_checks.version_bump_pigweed_pypi_distribution,
    shell_checks.shellcheck,
    # TODO(hepler): Many files are missing from the CMake build. Add this check
    # to lintformat when the missing files are fixed.
    source_in_build.cmake(upstream_checks.SOURCE_FILES_FILTER, _run_cmake),
    source_in_build.soong(upstream_checks.SOURCE_FILES_FILTER),
    static_analysis,
    stm32f429i,
    zephyr_build,
    # keep-sorted: end
)

ARDUINO_PICO = (
    # Skip gn_teensy_build if running on mac-arm64.
    # There are no arm specific tools packages available upstream:
    # https://www.pjrc.com/teensy/package_teensy_index.json
    (
        gn_teensy_build
        if not (sys.platform == 'darwin' and platform.machine() == 'arm64')
        else ()
    ),
    gn_pico_build,
    gn_pw_system_demo_build,
)

INTERNAL = (gn_mimxrt595_build, gn_mimxrt595_freertos_build)

SAPPHIRE = (
    # keep-sorted: start
    gn_chre_googletest_nanopb_sapphire_build,
    # keep-sorted: end
)

SANITIZERS = (cpp_checks.all_sanitizers(),)

SECURITY = (
    # keep-sorted: start
    gn_crypto_mbedtls_build,
    gn_software_update_build,
    # keep-sorted: end
)

FUZZ = (gn_fuzz_build, oss_fuzz_build)

_LINTFORMAT = pigweed_local_presubmit.QUICK + (
    format_code.presubmit_checks(),
    build.bazel_lint,  # TODO: b/432484923 - Remove when added to Bazel checks
    source_is_in_cmake_build_warn_only,
    javascript_checks.eslint if shutil.which('npm') else (),
)

LINTFORMAT = (
    _LINTFORMAT,
    python_checks.check_python_versions,
)

QUICK = (
    _LINTFORMAT,
    gn_quick_build_check,
)

FULL = (
    _LINTFORMAT,
    gn_combined_build_check,
    bazel_build,
    python_checks.gn_python_check,
    python_checks.gn_python_test_coverage,
    python_checks.check_upstream_python_constraints,
    build_env_setup,
)

PROGRAMS = Programs(
    # keep-sorted: start
    arduino_pico=ARDUINO_PICO,
    full=FULL,
    fuzz=FUZZ,
    internal=INTERNAL,
    lintformat=LINTFORMAT,
    other_checks=OTHER_CHECKS,
    quick=QUICK,
    sanitizers=SANITIZERS,
    sapphire=SAPPHIRE,
    security=SECURITY,
    # keep-sorted: end
)


def parse_args() -> argparse.Namespace:
    """Creates an argument parser and parses arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    cli.add_arguments(parser, PROGRAMS, 'quick')
    parser.add_argument(
        '--install',
        action='store_true',
        help='Install the presubmit as a Git pre-push hook and exit.',
    )

    return parser.parse_args()


def run(install: bool, exclude: list[Pattern[str]], **presubmit_args) -> int:
    """Entry point for presubmit."""

    if install:
        install_git_hook(
            'pre-push',
            [
                'python',
                '-m',
                'pw_presubmit.pigweed_presubmit',
                '--base',
                'origin/main..HEAD',
                '--program',
                'quick',
            ],
        )
        return 0

    exclude.extend(pigweed_local_presubmit.EXCLUDES)
    return cli.run(exclude=exclude, **presubmit_args)


def main() -> int:
    """Run the presubmit for the Pigweed repository."""
    return run(**vars(parse_args()))


if __name__ == '__main__':
    try:
        # If pw_cli is available, use it to initialize logs.
        from pw_cli import log  # pylint: disable=ungrouped-imports

        log.install(logging.INFO)
    except ImportError:
        # If pw_cli isn't available, display log messages like a simple print.
        logging.basicConfig(format='%(message)s', level=logging.INFO)

    sys.exit(main())
