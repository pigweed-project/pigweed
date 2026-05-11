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
"""Common build helpers used by build.py and build_with_pw_package.py.

This module is private and should not be used outside of pw_presubmit.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import logging
from pathlib import Path
import sys
from typing import Callable, ContextManager, Sequence

from pw_build.build_recipe import gn_args
import pw_cli.color
import pw_cli.env
from pw_presubmit.presubmit import call
from pw_presubmit.presubmit_context import PresubmitContext, LuciTrigger
from pw_presubmit.private.result import PresubmitFailure, PresubmitResult
from pw_presubmit.tools import log_run
from pw_presubmit import ninja_parser

_LOG = logging.getLogger(__name__)

# Type aliases moved from build.py and build_with_pw_package.py
Item = int | str
Value = Item | Sequence[Item]
ValueCallable = Callable[[PresubmitContext], Value]
InputItem = Item | ValueCallable
InputValue = InputItem | Sequence[InputItem]

_CtxMgrLambda = Callable[[PresubmitContext], ContextManager]
_CtxMgrOrLambda = ContextManager | _CtxMgrLambda


def _value(ctx: PresubmitContext, val: InputValue) -> Value:
    """Process any lambdas inside val"""
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


@dataclass(frozen=True)
class CommonCoverageOptions:
    target_bucket_root: str
    target_bucket_project: str
    trace_type: str
    owner: str
    bug_component: str


@dataclass(frozen=True)
class CodeSearchCoverageOptions:
    host: str
    project: str
    ref: str
    source: str
    add_prefix: str = ''


@dataclass(frozen=True)
class GerritCoverageOptions:
    project: str


@dataclass(frozen=True)
class CoverageOptions:
    common: CommonCoverageOptions
    codesearch: tuple[CodeSearchCoverageOptions, ...]
    gerrit: GerritCoverageOptions


def _copy_to_gcs(ctx: PresubmitContext, filepath: Path, gcs_dst: str):
    luci = Path(pw_cli.env.pigweed_environment().PW_LUCI_CIPD_INSTALL_DIR)
    gsutil = luci / 'gsutil' / 'gsutil'

    cmd = [gsutil, 'cp', filepath, gcs_dst]

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


# Functions moved from build.py
def gn_gen(
    ctx: PresubmitContext,
    *args: str,
    gn_check: bool = True,  # pylint: disable=redefined-outer-name
    gn_fail_on_unused: bool = True,
    export_compile_commands: bool | str = True,
    preserve_args_gn: bool = False,
    **gn_arguments,
) -> None:
    """Runs gn gen in the specified directory with optional GN args."""
    all_gn_args = {'pw_build_COLORIZE_OUTPUT': pw_cli.color.is_enabled()}
    all_gn_args.update(gn_arguments)
    all_gn_args.update(ctx.override_gn_args)
    _LOG.debug('%r', all_gn_args)
    args_option = gn_args(**all_gn_args)

    if not ctx.dry_run and not preserve_args_gn:
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
            ctx.fail(failure)

        raise exc
