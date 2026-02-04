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
"""General non-build presubmit checks only applicable to upstream Pigweed."""

from pathlib import Path
import logging
import re
import shutil
from typing import Iterable, Sequence, TextIO

from pw_cli.file_filter import FileFilter
from pw_cli.plural import plural

from pw_presubmit.presubmit_context import PresubmitContext, PresubmitFailure
from pw_presubmit import (
    bazel_checks,
    format_code,
    git_repo,
    inclusive_language,
    owners_checks,
    source_in_build,
    todo_check,
)
from pw_presubmit.presubmit import Check, filter_paths


_LOG = logging.getLogger('pw_presubmit')

BUILD_FILE_FILTER = FileFilter(
    suffix=(
        *format_code.C_FORMAT.extensions,
        '.cfg',
        '.py',
        '.rst',
        '.gn',
        '.gni',
        '.emb',
    )
)


SOURCE_FILES_FILTER = FileFilter(
    endswith=BUILD_FILE_FILTER.endswith,
    suffix=('.bazel', '.bzl', '.gn', '.gni', *BUILD_FILE_FILTER.suffix),
    exclude=(
        r'zephyr.*',
        r'android.*',
        r'\.black.toml',
        r'\bpyproject.toml',
    ),
)

SOURCE_FILES_FILTER_BAZEL_EXCLUDE = FileFilter(
    exclude=(
        # keep-sorted: start
        r'\bpw_docgen/py/tests/',
        # keep-sorted: end
    ),
)

SOURCE_FILES_FILTER_GN_EXCLUDE = FileFilter(
    exclude=(
        # keep-sorted: start
        r'.*\.rst$',
        r'\bcodelab/',
        r'\bdocs/',
        r'\bexamples/',
        r'\bjava/',
        r'\bjava_jni/',
        r'\bpw_bluetooth_sapphire/fuchsia/',
        r'\bpw_ide/bazel/',
        r'\bpw_kernel/',
        r'\bsize_report/',
        # keep-sorted: end
    ),
)


def _valid_capitalization(word: str) -> bool:
    """Checks that the word has a capital letter or is not a regular word."""
    return bool(
        any(c.isupper() for c in word)  # Any capitalizatian (iTelephone)
        or not word.isalpha()  # Non-alphabetical (cool_stuff.exe)
        or shutil.which(word)
    )  # Matches an executable (clangd)


def commit_message_format(ctx: PresubmitContext):
    """Checks that the top commit's message is correctly formatted."""
    if git_repo.commit_author().endswith('gserviceaccount.com'):
        return

    lines = git_repo.commit_message().splitlines()

    # Ignore fixup/squash commits, but only if running locally.
    if not ctx.luci and lines[0].startswith(('fixup!', 'squash!')):
        return

    # Show limits and current commit message in log.
    _LOG.debug('%-25s%+25s%+22s', 'Line limits', '72|', '72|')
    for line in lines:
        _LOG.debug(line)

    if not lines:
        _LOG.error('The commit message is too short!')
        raise PresubmitFailure

    # Ignore merges.
    repo = git_repo.LoggingGitRepo(Path.cwd())
    parents = repo.commit_parents()
    _LOG.debug('parents: %r', parents)
    if len(parents) > 1:
        _LOG.warning('Ignoring multi-parent commit')
        return

    # Ignore Gerrit-generated reverts.
    if (
        'Revert' in lines[0]
        and 'This reverts commit ' in git_repo.commit_message()
        and 'Reason for revert: ' in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring apparent Gerrit-generated revert')
        return

    # Ignore Gerrit-generated relands
    if (
        'Reland' in lines[0]
        and 'This is a reland of ' in git_repo.commit_message()
        and "Original change's description:" in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring apparent Gerrit-generated reland')
        return

    errors = 0

    # Check that the first line matches the expected pattern.
    match = re.match(
        r'^(?P<prefix>[.\w*/]+(?:{[\w* ,]+})?[\w*/]*|SEED-\d+|clang-\w+): '
        r'(?P<desc>.+)$',
        lines[0],
    )

    if len(lines[0]) > 72:
        if match and match.group('prefix') == 'roll':
            _LOG.debug('Allowing long first line for roll commit')
        else:
            _LOG.warning(
                "The commit message's first line must be no longer than "
                '72 characters.'
            )
            _LOG.warning(
                'The first line is %d characters:\n  %s',
                len(lines[0]),
                lines[0],
            )
            errors += 1

    if lines[0].endswith('.'):
        _LOG.warning(
            "The commit message's first line must not end with a period:\n %s",
            lines[0],
        )
        errors += 1

    if not match:
        _LOG.warning('The first line does not match the expected format')
        _LOG.warning(
            'Expected:\n\n  module_or_target: The description\n\n'
            'Found:\n\n  %s\n',
            lines[0],
        )
        errors += 1
    elif match.group('prefix') == 'roll':
        # We're much more flexible with roll commits.
        pass
    elif not _valid_capitalization(match.group('desc').split()[0]):
        _LOG.warning(
            'The first word after the ":" in the first line ("%s") must be '
            'capitalized:\n  %s',
            match.group('desc').split()[0],
            lines[0],
        )
        errors += 1

    if len(lines) > 1 and lines[1]:
        _LOG.warning("The commit message's second line must be blank.")
        _LOG.warning(
            'The second line has %d characters:\n  %s', len(lines[1]), lines[1]
        )
        errors += 1

    # Ignore the line length check for Copybara imports so they can include the
    # commit hash and description for imported commits.
    if not errors and (
        'Copybara import' in lines[0]
        and 'GitOrigin-RevId:' in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring Copybara import')
        return

    # Check that the lines are 72 characters or less.
    for i, line in enumerate(lines[2:], 3):
        # Skip any lines that might possibly have a URL, path, or metadata in
        # them.
        if any(c in line for c in ':/>'):
            continue

        # Skip any lines with non-ASCII characters.
        if not line.isascii():
            continue

        # Skip any blockquoted lines.
        if line.startswith('  '):
            continue

        if len(line) > 72:
            _LOG.warning(
                'Commit message lines must be no longer than 72 characters.'
            )
            _LOG.warning('Line %d has %d characters:\n  %s', i, len(line), line)
            errors += 1

    if errors:
        _LOG.error('Found %s in the commit message', plural(errors, 'error'))
        raise PresubmitFailure


_EXCLUDE_FROM_COPYRIGHT_NOTICE: Sequence[str] = (
    # Configuration
    # keep-sorted: start
    r'MODULE.bazel.lock',
    r'\b49-pico.rules$',
    r'\bCargo.lock$',
    r'\bDoxyfile$',
    r'\bPW_PLUGINS$',
    r'\bconstraint.list$',
    r'\bconstraint_hashes_darwin.list$',
    r'\bconstraint_hashes_linux.list$',
    r'\bconstraint_hashes_windows.list$',
    r'\bpython_base_requirements.txt$',
    r'\bupstream_requirements_darwin_lock.txt$',
    r'\bupstream_requirements_linux_lock.txt$',
    r'\bupstream_requirements_windows_lock.txt$',
    r'^(?:.+/)?\.bazelversion$',
    r'^pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version',
    # keep-sorted: end
    # Metadata
    # keep-sorted: start
    r'\b.*OWNERS.*$',
    r'\bAUTHORS$',
    r'\bLICENSE$',
    r'\bPIGWEED_MODULES$',
    r'\b\.vscodeignore$',
    r'\bgo.(mod|sum)$',
    r'\bpackage-lock.json$',
    r'\bpackage.json$',
    r'\bpnpm-lock.yaml$',
    r'\brequirements.txt$',
    r'\byarn.lock$',
    r'^docker/tag$',
    r'^patches.json$',
    # keep-sorted: end
    # Data files
    # keep-sorted: start
    r'\.bin$',
    r'\.csv$',
    r'\.elf$',
    r'\.gif$',
    r'\.ico$',
    r'\.jpg$',
    r'\.json$',
    r'\.png$',
    r'\.svg$',
    r'\.vsix$',
    r'\.woff2',
    r'\.xml$',
    # keep-sorted: end
    # Documentation
    # keep-sorted: start
    r'\.expected$',
    r'\.md$',
    r'\.rst$',
    r'^docs/sphinx/changelog/py/templates/.*\.tmpl$',
    # TODO: b/388905812 - Delete this file.
    r'^docs/sphinx/size_report_notice$',
    # keep-sorted: end
    # Generated protobuf files
    # keep-sorted: start
    r'\.pb\.c$',
    r'\.pb\.h$',
    r'\_pb2.pyi?$',
    # keep-sorted: end
    # Generated third-party files
    # keep-sorted: start
    r'\bthird_party/.*\.bazelrc$',
    r'\bthird_party/fuchsia/repo',
    r'\bthird_party/perfetto/repo/protos/perfetto/trace/perfetto_trace.proto',
    # keep-sorted: end
    # Diff/Patch files
    # keep-sorted: start
    r'\.diff$',
    r'\.patch$',
    # keep-sorted: end
    # Test data
    # keep-sorted: start
    r'^pw_build/test_data/pw_copy_and_patch_file/',
    r'^pw_build/test_data/test_runfile\.txt$',
    r'^pw_presubmit/py/test/owners_checks/',
    # keep-sorted: end
)

# Regular expression for the copyright comment. "\1" refers to the comment
# characters and "\2" refers to space after the comment characters, if any.
# All period characters are escaped using a replace call.
# pylint: disable=line-too-long
_COPYRIGHT = re.compile(
    r"""(#|//|::| \*|)( ?)Copyright 2\d{3} The Pigweed Authors
\1
\1\2Licensed under the Apache License, Version 2.0 \(the "License"\); you may not
\1\2use this file except in compliance with the License. You may obtain a copy of
\1\2the License at
\1
\1(?:\2    |\t)https://www.apache.org/licenses/LICENSE-2.0
\1
\1\2Unless required by applicable law or agreed to in writing, software
\1\2distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
\1\2WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
\1\2License for the specific language governing permissions and limitations under
\1\2the License.
""".replace(
        '.', r'\.'
    ),
    re.MULTILINE,
)
# pylint: enable=line-too-long

_SKIP_LINE_PREFIXES = (
    '#!',
    '#autoload',
    '#compdef',
    '@echo off',
    ':<<',
    '/*',
    ' * @jest-environment jsdom',
    ' */',
    '{#',  # Jinja comment block
    '# -*- coding: utf-8 -*-',
    '<!--',
)


def _read_notice_lines(file: TextIO) -> Iterable[str]:
    lines = iter(file)
    try:
        # Read until the first line of the copyright notice.
        line = next(lines)
        while line.isspace() or line.startswith(_SKIP_LINE_PREFIXES):
            line = next(lines)

        yield line

        for _ in range(12):  # The notice is 13 lines; read the remaining 12.
            yield next(lines)
    except StopIteration:
        pass


@filter_paths(exclude=_EXCLUDE_FROM_COPYRIGHT_NOTICE)
def copyright_notice(ctx: PresubmitContext):
    """Checks that the Pigweed copyright notice is present."""
    errors = []

    for path in ctx.paths:
        if path.stat().st_size == 0:
            continue  # Skip empty files

        try:
            with path.open() as file:
                if not _COPYRIGHT.match(''.join(_read_notice_lines(file))):
                    errors.append(path)
        except UnicodeDecodeError as exc:
            raise PresubmitFailure(f'failed to read {path}') from exc

    if errors:
        _LOG.warning(
            '%s with a missing or incorrect copyright notice:\n%s',
            plural(errors, 'file'),
            '\n'.join(str(e) for e in errors),
        )
        raise PresubmitFailure


@filter_paths(file_filter=format_code.OWNERS_CODE_FORMAT.filter)
def owners_lint_checks(ctx: PresubmitContext):
    """Runs OWNERS linter."""
    owners_checks.presubmit_check(ctx.paths)


# cc_library targets which contain the forbidden `includes` attribute.
#
# TODO: https://pwbug.dev/378564135 - Burn this list down.
INCLUDE_CHECK_EXCEPTIONS = (
    # keep-sorted: start
    "//pw_assert_log:check_and_assert_backend",
    "//pw_async_fuchsia:dispatcher",
    "//pw_async_fuchsia:fake_dispatcher",
    "//pw_async_fuchsia:task",
    "//pw_async_fuchsia:util",
    "//pw_bluetooth:emboss_att",
    "//pw_bluetooth:emboss_avdtp",
    "//pw_bluetooth:emboss_hci_android",
    "//pw_bluetooth:emboss_hci_commands",
    "//pw_bluetooth:emboss_hci_common",
    "//pw_bluetooth:emboss_hci_data",
    "//pw_bluetooth:emboss_hci_events",
    "//pw_bluetooth:emboss_hci_h4",
    "//pw_bluetooth:emboss_hci_test",
    "//pw_bluetooth:emboss_l2cap_frames",
    "//pw_bluetooth:emboss_rfcomm_frames",
    "//pw_bluetooth:emboss_snoop",
    "//pw_bluetooth:emboss_util",
    "//pw_bluetooth:pw_bluetooth",
    "//pw_bluetooth:pw_bluetooth2",
    "//pw_bluetooth:snoop",
    "//pw_bluetooth_sapphire:peripheral",
    "//pw_build/bazel_internal:header_test",
    "//pw_chrono_embos:system_clock",
    "//pw_chrono_embos:system_timer",
    "//pw_chrono_freertos:system_clock",
    "//pw_chrono_freertos:system_timer",
    "//pw_chrono_rp2040:system_clock",
    "//pw_chrono_stl:system_clock",
    "//pw_chrono_stl:system_timer",
    "//pw_chrono_threadx:system_clock",
    "//pw_cpu_exception_cortex_m:cpu_exception",
    "//pw_cpu_exception_cortex_m:crash_test.lib",
    "//pw_crypto:aes",
    "//pw_crypto:aes.facade",
    "//pw_crypto:aes_boringssl",
    "//pw_crypto:aes_mbedtls",
    "//pw_crypto:sha256_mbedtls",
    "//pw_crypto:sha256_mock",
    "//pw_fuzzer/examples/fuzztest:metrics_lib",
    "//pw_fuzzer:fuzztest",
    "//pw_fuzzer:fuzztest_stub",
    "//pw_ide/bazel/compile_commands/test:basic_library_direct_includes",
    "//pw_interrupt_cortex_m:context",
    "//pw_log_fuchsia:pw_log_fuchsia",
    "//pw_log_null:headers",
    "//pw_metric:metric_service_pwpb",
    "//pw_multibuf/examples:emboss_protocol",
    "//pw_perf_test:arm_cortex_timer",
    "//pw_perf_test:chrono_timer",
    "//pw_polyfill:standard_library",
    "//pw_rpc:internal_test_utils",
    "//pw_sensor:pw_sensor_types",
    "//pw_sync:binary_semaphore_thread_notification_backend",
    "//pw_sync:binary_semaphore_timed_thread_notification_backend",
    "//pw_sync_baremetal:interrupt_spin_lock",
    "//pw_sync_baremetal:mutex",
    "//pw_sync_baremetal:recursive_mutex",
    "//pw_sync_embos:binary_semaphore",
    "//pw_sync_embos:counting_semaphore",
    "//pw_sync_embos:interrupt_spin_lock",
    "//pw_sync_embos:mutex",
    "//pw_sync_embos:timed_mutex",
    "//pw_sync_freertos:binary_semaphore",
    "//pw_sync_freertos:counting_semaphore",
    "//pw_sync_freertos:interrupt_spin_lock",
    "//pw_sync_freertos:mutex",
    "//pw_sync_freertos:thread_notification",
    "//pw_sync_freertos:timed_mutex",
    "//pw_sync_freertos:timed_thread_notification",
    "//pw_sync_threadx:binary_semaphore",
    "//pw_sync_threadx:counting_semaphore",
    "//pw_sync_threadx:interrupt_spin_lock",
    "//pw_sync_threadx:mutex",
    "//pw_sync_threadx:timed_mutex",
    "//pw_system:freertos_target_hooks",
    "//pw_thread_embos:id",
    "//pw_thread_embos:sleep",
    "//pw_thread_embos:thread",
    "//pw_thread_embos:yield",
    "//pw_thread_threadx:id",
    "//pw_thread_threadx:sleep",
    "//pw_thread_threadx:thread",
    "//pw_thread_threadx:yield",
    "//pw_tls_client_boringssl:pw_tls_client_boringssl",
    "//pw_tls_client_mbedtls:pw_tls_client_mbedtls",
    "//pw_trace:null",
    "//pw_trace:pw_trace_sample_app",
    "//pw_trace:trace_facade_test.lib",
    "//pw_trace:trace_zero_facade_test.lib",
    "//pw_trace_tokenized:pw_trace_tokenized",
    "//pw_unit_test:rpc_service",
    "//third_party/fuchsia:fit_impl",
    "//third_party/fuchsia:stdcompat",
    # keep-sorted: end
)

INCLUDE_CHECK_TARGET_PATTERN = "//... " + " ".join(
    "-" + target for target in INCLUDE_CHECK_EXCEPTIONS
)


def bazel_includes() -> Check:
    return bazel_checks.includes_presubmit_check(INCLUDE_CHECK_TARGET_PATTERN)


_EXCLUDE_FROM_TODO_CHECK = (
    # keep-sorted: start
    r'.bazelrc$',
    r'.dockerignore$',
    r'.gitignore$',
    r'.pylintrc$',
    r'.ruff.toml$',
    r'MODULE.bazel.lock$',
    r'\bdocs/build_system.rst',
    r'\bdocs/code_reviews.rst',
    r'\bpw_assert_basic/basic_handler.cc',
    r'\bpw_assert_basic/public/pw_assert_basic/handler.h',
    r'\bpw_blob_store/public/pw_blob_store/flat_file_system_entry.h',
    r'\bpw_build/linker_script.gni',
    r'\bpw_build/py/pw_build/copy_from_cipd.py',
    r'\bpw_cpu_exception/basic_handler.cc',
    r'\bpw_cpu_exception_cortex_m/entry.cc',
    r'\bpw_cpu_exception_cortex_m/exception_entry_test.cc',
    r'\bpw_doctor/py/pw_doctor/doctor.py',
    r'\bpw_env_setup/util.sh',
    r'\bpw_fuzzer/fuzzer.gni',
    r'\bpw_i2c/BUILD.gn',
    r'\bpw_i2c/public/pw_i2c/register_device.h',
    r'\bpw_kernel/.*',
    r'\bpw_kvs/flash_memory.cc',
    r'\bpw_kvs/key_value_store.cc',
    r'\bpw_log_basic/log_basic.cc',
    r'\bpw_package/py/pw_package/packages/chromium_verifier.py',
    r'\bpw_protobuf/encoder.cc',
    r'\bpw_rpc/docs.rst',
    r'\bpw_watch/py/pw_watch/watch.py',
    r'\btargets/mimxrt595_evk/BUILD.bazel',
    r'\btargets/stm32f429i_disc1/boot.cc',
    r'\bthird_party/chromium_verifier/BUILD.gn',
    # keep-sorted: end
)


@filter_paths(exclude=_EXCLUDE_FROM_TODO_CHECK)
def todo_check_with_exceptions(ctx: PresubmitContext):
    """Check that non-legacy TODO lines are valid."""  # todo-check: ignore
    todo_check.create(todo_check.BUGS_OR_USERNAMES)(ctx)


def source_in_gn_build() -> Check:
    return source_in_build.gn(SOURCE_FILES_FILTER).with_file_filter(
        SOURCE_FILES_FILTER_GN_EXCLUDE
    )


def source_in_bazel_build() -> Check:
    return source_in_build.bazel(SOURCE_FILES_FILTER).with_file_filter(
        SOURCE_FILES_FILTER_BAZEL_EXCLUDE
    )


inclusive_language_check = inclusive_language.presubmit_check.with_filter(
    exclude=(
        r'\bMODULE.bazel.lock$',
        r'\bgo.sum$',
        r'\bpackage-lock.json$',
        r'\bpnpm-lock.yaml$',
        r'\byarn.lock$',
    )
)
