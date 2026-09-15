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
"""Defines the generic Scanner base class for the pw_fortifier pipeline."""

import argparse
import asyncio
import logging
import os
from pathlib import Path
import shutil
import sys
import tempfile
from abc import ABC, abstractmethod
from typing import TYPE_CHECKING

from pw_fortifier.emitter import Emitter
from pw_fortifier.git_utils import (
    ReadOnlyGitWorkspace,
    WritableGitWorkspace,
    get_git_repo_root,
)
from pw_fortifier.issue_tracker import IssueReader, IssueWriter
from pw_fortifier.pipeline_stage import (
    BasicPipelineStage,
    PipelineDemux,
    PipelineProducerMixin,
    PipelineConsumerMixin,
    PipelineStage,
    DEFAULT_MAX_RETRIES,
)

if TYPE_CHECKING:
    from pw_fortifier.collector import Collector
    from pw_fortifier.deduplicator import Deduplicator
    from pw_fortifier.triager import Triager


class Scanner(ABC):
    """Generic scanner that orchestrates a pipeline of stages."""

    DESC: str | None = None

    # pylint: disable=too-many-instance-attributes
    def __init__(self, name: str) -> None:
        """Initializes the Scanner with a program name.

        Args:
            name: Name of the scanner program.
        """
        self.repo_url: str | None = None

        self._name = Path(name).stem
        self._stages: list[BasicPipelineStage] = []
        self._emitter = Emitter()
        self._deduplicator: Deduplicator | None = None
        self._triager: Triager | None = None
        self._issue_writer: IssueWriter | None = None
        self._issue_reader: IssueReader | None = None
        self._code_generator: PipelineStage | None = None
        self._collector: Collector | None = None

    async def run(self, *cli_args) -> None:
        """Creates and runs the pipeline stages concurrently.

        Args:
            *cli_args: Command-line arguments to parse.
        """
        args = self._parse_args(*cli_args)
        self._setup_logging(args)
        self._instantiate(args)
        await self._configure(args)
        tasks = [asyncio.create_task(stage.run()) for stage in self._stages]
        await asyncio.gather(*tasks)

    @staticmethod
    def _setup_logging(args: argparse.Namespace) -> None:
        """Configures root logging based on CLI arguments."""
        level = logging.DEBUG if args.verbose else logging.WARNING
        log_format = '%(asctime)s [%(levelname)s] %(name)s: %(message)s'
        if args.errors:
            log_path = Path(args.errors)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            logging.basicConfig(
                filename=str(log_path),
                filemode='a',
                level=level,
                format=log_format,
                force=True,
            )
        else:
            logging.basicConfig(
                stream=sys.stderr,
                level=level,
                format=log_format,
                force=True,
            )

    def _instantiate(self, args: argparse.Namespace) -> None:
        """Instantiates all pipeline stages.

        Args:
            args: Command-line arguments namespace.
        """
        del args  # Unused in base class.
        self._add_stage(self._emitter)

        (first_gen_stage, last_gen_stage) = self._add_generator_stages()
        self._emitter.connect(first_gen_stage)

        assert self._deduplicator is not None
        self._add_stage(self._deduplicator)
        last_gen_stage.connect(self._deduplicator)

        assert self._triager is not None
        self._add_stage(self._triager)
        self._deduplicator.connect(self._triager)

        assert self._issue_writer is not None
        self._add_stage(self._issue_writer)
        self._triager.connect(self._issue_writer)

        issue_demux = PipelineDemux('issue_demux')
        self._add_stage(issue_demux)
        issue_demux.add_stage(self._issue_writer)

        assert self._issue_reader is not None
        self._add_stage(self._issue_reader)
        issue_demux.add_stage(self._issue_reader)

        assert self._code_generator
        self._add_stage(self._code_generator)
        issue_demux.connect(self._code_generator)

        assert self._collector is not None
        self._add_stage(self._collector)
        self._code_generator.connect(self._collector)

    def _add_stage(self, stage: BasicPipelineStage) -> None:
        """Adds a stage to the pipeline."""
        self._stages.append(stage)

    def _parse_args(self, *cli_args) -> argparse.Namespace:
        """Parses command-line arguments."""
        parser = argparse.ArgumentParser(
            prog=self._name,
            description=self.DESC,
        )
        parser.add_argument(
            '-d',
            '--dst-repo',
            type=str,
            required=False,
            help=(
                'Path to a writable repository to update. '
                'A temporary clone of the repo is used if omitted.'
            ),
        )
        parser.add_argument(
            '-f',
            '--files',
            nargs='*',
            type=str,
            help=(
                'Specific files to scan instead of enumerating the repo. '
                'Filenames may contain wildcards.'
            ),
        )
        parser.add_argument(
            '-i',
            '--issue',
            '--issue-id',
            dest='issues',
            action='append',
            type=int,
            default=[],
            help=(
                'Issue IDs to use instead of issues found by analyzing files.'
            ),
        )
        parser.add_argument(
            '-l',
            '--hotlist',
            '--hotlist-id',
            dest='hotlists',
            action='append',
            type=int,
            default=[],
            help=(
                'Hotlist IDs to use instead of issues found by analyzing files.'
            ),
        )
        parser.add_argument(
            '-b',
            '--create-bugs',
            action='store_true',
            help=(
                'Create Buganizer issues for findings '
                '(defaults to printing to stdout).'
            ),
        )
        parser.add_argument(
            '-c',
            '--clean',
            action='store_true',
            help=('Remove everything in the working directory before running.'),
        )
        parser.add_argument(
            '-e',
            '--allow-edits',
            action='store_true',
            help=(
                'Create or update local Git revisions and run validation'
                ' builds.'
            ),
        )
        parser.add_argument(
            '-E',
            '--errors',
            type=str,
            required=False,
            default=None,
            help=(
                'Path to a file where diagnostic and error logs will be written'
                ' (defaults to stderr).'
            ),
        )
        parser.add_argument(
            '-m',
            '--max-retries',
            type=int,
            default=DEFAULT_MAX_RETRIES,
            help='Max retries for failed pipeline steps.',
        )
        parser.add_argument(
            '-o',
            '--output',
            type=str,
            required=False,
            help=(
                'Path to a CSV file where the final stage results will be'
                ' written.'
            ),
        )
        parser.add_argument(
            '-r',
            '--resume',
            action='store_true',
            help='Use intermediate results from an interrupted run.',
        )
        parser.add_argument(
            '-s',
            '--src-repo',
            type=str,
            required=False,
            help=(
                'Path to a read-only repository to scan. Defaults to the'
                ' current repository, or a temporary clone if omitted.'
            ),
        )
        parser.add_argument(
            '-u',
            '--allow-uploads',
            action='store_true',
            help='Create or update Gerrit CLs (implies -e/--allow-edits).',
        )
        parser.add_argument(
            '-v',
            '--verbose',
            action='store_true',
            help='Enable verbose output.',
        )
        default_working_dir = str(Path(tempfile.gettempdir(), self._name))
        parser.add_argument(
            '-w',
            '--working-dir',
            type=str,
            default=default_working_dir,
            help=(
                'Working directory for intermediate results '
                f'(defaults to {default_working_dir}).'
            ),
        )

        args = parser.parse_args(list(cli_args) if cli_args else None)
        if args.allow_uploads:
            args.allow_edits = True

        return args

    async def _configure(self, args: argparse.Namespace) -> None:
        """Configures the pipeline stages."""
        working_dir = Path(args.working_dir)
        if args.clean and working_dir.exists():
            for item in working_dir.iterdir():
                if item.is_dir():
                    shutil.rmtree(item)
                else:
                    item.unlink()
        working_dir.mkdir(parents=True, exist_ok=True)

        if args.issues or args.hotlists:
            args.files = []

        if args.src_repo:
            src_repo_path = Path(args.src_repo)
            if (
                not src_repo_path.is_absolute()
                and 'BUILD_WORKING_DIRECTORY' in os.environ
            ):
                src_repo_path = (
                    Path(os.environ['BUILD_WORKING_DIRECTORY']) / src_repo_path
                )
            src_repo = ReadOnlyGitWorkspace(src_repo_path)
        elif (current_repo := get_git_repo_root()) is not None:
            src_repo = ReadOnlyGitWorkspace(current_repo)
        else:
            assert self.repo_url is not None
            src_repo = await ReadOnlyGitWorkspace.clone(
                self.repo_url,
                working_dir / 'src',
                git_filter='blob:none',
                depth=None,
            )

        dst_repo = None
        if args.dst_repo:
            dst_repo_path = Path(args.dst_repo)
            if (
                not dst_repo_path.is_absolute()
                and 'BUILD_WORKING_DIRECTORY' in os.environ
            ):
                dst_repo_path = (
                    Path(os.environ['BUILD_WORKING_DIRECTORY']) / dst_repo_path
                )
            dst_repo = WritableGitWorkspace(dst_repo_path)
        else:
            assert self.repo_url is not None
            dst_repo = await WritableGitWorkspace.clone(
                self.repo_url,
                working_dir / 'dst',
            )

        for stage in self._stages:
            stage.src_repo = src_repo
            stage.dst_repo = dst_repo
            await stage.configure(args)

    @abstractmethod
    def _add_generator_stages(
        self,
    ) -> tuple[PipelineConsumerMixin, PipelineProducerMixin]:
        """Instantiates pipeline stages used to create issues.

        Derived types should use `_add_stage` to register any stages needed to
        generate issues.

        Returns:
            A tuple of the first and last stages added.
        """
        raise NotImplementedError


################################################################################
# Test support


async def configure_stage_for_test(stage: BasicPipelineStage, **kwargs) -> None:
    """Configures a pipeline stage with mock settings for testing.

    Args:
        stage: The pipeline stage to configure.
        **kwargs: Configuration arguments such as working_dir.
    """
    working_dir = kwargs['working_dir']
    kwargs.setdefault('dst_repo', str(Path(working_dir, 'dst_repo')))
    kwargs.setdefault('files', None)
    kwargs.setdefault('issues', [])
    kwargs.setdefault('hotlists', [])
    kwargs.setdefault('max_retries', DEFAULT_MAX_RETRIES)
    kwargs.setdefault('create_bugs', False)
    kwargs.setdefault('clean', False)
    kwargs.setdefault('allow_edits', False)
    kwargs.setdefault('allow_uploads', False)
    kwargs.setdefault('errors', None)
    kwargs.setdefault('output', None)
    kwargs.setdefault('resume', False)
    kwargs.setdefault('src_repo', str(Path(working_dir, 'src_repo')))
    kwargs.setdefault('verbose', False)

    if kwargs.get('allow_uploads', False):
        kwargs['allow_edits'] = True

    src_repo = kwargs.get('src_repo')
    if isinstance(src_repo, ReadOnlyGitWorkspace):
        stage.src_repo = src_repo
    elif isinstance(src_repo, (str, os.PathLike)):
        src_repo_path = Path(src_repo)
        src_repo_path.mkdir(parents=True, exist_ok=True)
        stage.src_repo = ReadOnlyGitWorkspace(src_repo_path)

    dst_repo = kwargs.get('dst_repo')
    if isinstance(dst_repo, WritableGitWorkspace):
        stage.dst_repo = dst_repo
    elif isinstance(dst_repo, (str, os.PathLike)):
        dst_repo_path = Path(dst_repo)
        dst_repo_path.mkdir(parents=True, exist_ok=True)
        stage.dst_repo = WritableGitWorkspace(dst_repo_path)

    await stage.configure(argparse.Namespace(**kwargs))
