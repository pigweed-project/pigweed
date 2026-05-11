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
"""Build helpers that depend on pw_package."""

from __future__ import annotations

import contextlib
import itertools
import logging
import os
from pathlib import Path
import posixpath
import tarfile
from typing import Any, Callable, Iterator, Sequence

from pw_package import package_manager
from pw_presubmit.presubmit_context import PresubmitContext
from pw_presubmit.private.check import Check, SubStep
from pw_presubmit.private.result import PresubmitFailure, PresubmitResult
from pw_presubmit.private.build_common import (
    _value,
    CoverageOptions,
    _copy_to_gcs,
    _write_coverage_metadata,
    gn_gen,
    gn_check,
    ninja,
    _CtxMgrOrLambda,
)

_LOG = logging.getLogger(__name__)


def install_package(
    ctx: PresubmitContext,
    name: str,
    force: bool = False,
) -> None:
    """Install package with given name in given path."""
    root = ctx.package_root
    mgr = package_manager.PackageManager(root)

    ctx.append_check_command(
        'pw',
        'package',
        'install',
        name,
        call_annotation={'pw_package_install': name},
    )
    if ctx.dry_run:
        return

    if not mgr.list():
        raise PresubmitFailure(
            'no packages configured, please import your pw_package '
            'configuration module'
        )

    if not mgr.status(name) or force:
        mgr.install(name, force=force)


Item = int | str
Value = Item | Sequence[Item]
ValueCallable = Callable[[PresubmitContext], Value]
InputItem = Item | ValueCallable
InputValue = InputItem | Sequence[InputItem]


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

    @staticmethod
    def _install_package(
        ctx: PresubmitContext,
        package: str,
    ) -> PresubmitResult:
        install_package(ctx, package)
        return PresubmitResult.PASS

    @contextlib.contextmanager
    def _context(self, ctx: PresubmitContext):
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


class GnGenNinja(_NinjaBase):
    """Thin wrapper of Check for steps that just call gn/ninja.

    Runs gn gen, ninja, then gn check.
    """

    def __init__(
        self,
        *args,
        gn_args: dict[str, Any] | None = None,
        **kwargs,
    ):
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
        gn_gen(ctx, gn_check=False, **args)
        return PresubmitResult.PASS

    def _substeps(self) -> Iterator[SubStep]:
        yield from self._package_substeps()
        yield SubStep('gn gen', self._gn_gen)
        yield from self._ninja_substeps()
        yield SubStep('gn check', gn_check)
        yield from self._coverage_substeps()
