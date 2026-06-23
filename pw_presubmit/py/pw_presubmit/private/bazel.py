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
"""Run tests of affected bazel targets."""

import functools
import logging
from pathlib import Path
from typing import Sequence

from pw_presubmit.format.cpp import CPP_EXTS
from pw_presubmit.private import tools
from pw_presubmit.v2 import Context, PresubmitFailure, step

_LOG = logging.getLogger(__name__)


@functools.lru_cache(maxsize=1)
def _get_non_manual_affected_targets(
    files: tuple[Path, ...],
    root: Path,
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Finds affected non-manual rules and tests using bazel query. Cached."""
    if not files:
        return (), ()

    file_list = ' '.join(
        str(p.relative_to(root)) if p.is_absolute() else str(p) for p in files
    )

    query_expr = f'rdeps(//..., set({file_list}))'
    res = tools.run_subprocess(
        [
            'bazelisk',
            'query',
            '--output=label_kind',
            '--noimplicit_deps',
            '--keep_going',
            f'{query_expr} except attr("tags", "manual", {query_expr})',
        ],
        cwd=root,
        allowed_returncodes=(0, 3),
    )
    if res.returncode not in (0, 3):
        raise PresubmitFailure('bazel query failed')

    all_rules = []
    test_rules = []

    for line in res.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        line_parts = line.split()
        if len(line_parts) == 3 and line_parts[1] == 'rule':
            kind, _, label = line_parts
            all_rules.append(label)
            if kind.endswith('_test'):
                test_rules.append(label)

    return tuple(all_rules), tuple(test_rules)


def _get_affected_targets(
    files: tuple[Path, ...],
    root: Path,
    extra_tags: Sequence[str] = (),
) -> tuple[list[str], list[str]]:
    """Finds affected targets, filtering out manual and extra_tags."""
    all_rules, test_rules = _get_non_manual_affected_targets(files, root)
    if not extra_tags or not all_rules:
        return list(all_rules), list(test_rules)

    # Fast targeted query to find which of all_rules have any of the extra_tags.
    skip_tags = '|'.join(extra_tags)
    rules_list = ' '.join(all_rules)
    res = tools.run_subprocess(
        [
            'bazelisk',
            'query',
            '--keep_going',
            f'attr("tags", "{skip_tags}", set({rules_list}))',
        ],
        cwd=root,
        allowed_returncodes=(0, 3),
    )
    if res.returncode not in (0, 3):
        raise PresubmitFailure('bazel query failed')

    tagged_rules = set(
        line.strip() for line in res.stdout.splitlines() if line.strip()
    )

    filtered_all = [r for r in all_rules if r not in tagged_rules]
    filtered_test = [r for r in test_rules if r not in tagged_rules]

    return filtered_all, filtered_test


def _run_bazel_command(
    ctx: Context,
    cmd: str,
    rules: Sequence[str],
    extra_args: Sequence[str] = (),
) -> None:
    """Helper to run a bazelisk command on rules."""
    if not rules:
        return

    res = tools.run_subprocess(
        [
            'bazelisk',
            cmd,
            '--keep_going',
            '--skip_incompatible_explicit_targets',
            '--noshow_progress',
            '--noshow_loading_progress',
        ]
        + list(extra_args)
        + list(rules),
        cwd=ctx.root,
    )
    if res.returncode != 0:
        raise PresubmitFailure(f'bazel {cmd} failed')


@step()
def build_affected_targets(ctx: Context) -> None:
    """Builds affected targets using bazelisk."""
    if not ctx.paths:
        return

    all_rules, _ = _get_affected_targets(ctx.all_modified_paths, ctx.root)
    _run_bazel_command(ctx, 'build', all_rules)


@step()
def test_affected_targets(ctx: Context) -> None:
    """Runs affected tests using bazelisk."""
    if not ctx.paths:
        return

    _, test_rules = _get_affected_targets(ctx.all_modified_paths, ctx.root)
    _run_bazel_command(ctx, 'test', test_rules)


@step(suffix=('.py',))
def python_lint(ctx: Context) -> None:
    """Runs mypy and pylint on affected targets using bazelisk."""
    if not ctx.paths:
        return

    mypy_rules, _ = _get_affected_targets(
        ctx.all_modified_paths, ctx.root, extra_tags=('nomypy',)
    )
    _run_bazel_command(
        ctx,
        'build',
        mypy_rules,
        extra_args=('--config=mypy',),
    )

    pylint_rules, _ = _get_affected_targets(
        ctx.all_modified_paths, ctx.root, extra_tags=('nopylint',)
    )
    _run_bazel_command(
        ctx,
        'build',
        pylint_rules,
        extra_args=('--config=pylint',),
    )


@step(suffix=CPP_EXTS)
def clang_tidy(ctx: Context) -> None:
    """Runs clang-tidy on affected targets using bazelisk."""
    if not ctx.paths:
        return

    all_rules, _ = _get_affected_targets(
        ctx.all_modified_paths, ctx.root, extra_tags=('noclangtidy',)
    )
    _run_bazel_command(
        ctx,
        'build',
        all_rules,
        extra_args=('--config=clang-tidy',),
    )
