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
"""Plugin used for pw build subcommands."""

import argparse
from collections.abc import Sequence
import fnmatch
import functools
import itertools
import logging
from pathlib import Path
import sys

from google.protobuf import text_format
import pw_cli.color
from pw_cli import multitool

from pw_build.build_recipe import BuildRecipe
from pw_build import project_builder
from pw_build.project_builder_argparse import (
    add_project_builder_option_arguments,
)
from pw_build.workflows.manager import WorkflowsManager

_COLOR = pw_cli.color.colors()
_LOG = logging.getLogger(__name__)
_PROJECT_BUILDER_LOGGER = logging.getLogger(f'{_LOG.name}.project_builder')


class WorkflowBuildPlugin(multitool.MultitoolPlugin):
    """Plugin to handle argparse and running builds for ./pw build."""

    def __init__(
        self,
        manager: WorkflowsManager | None,
        artifacts_manifest: Path | None = None,
    ):
        self._manager = manager
        if self._manager is None:
            raise AssertionError(
                'Internal error: failed to initialize workflows manager'
            )
        self._artifacts_manifest = artifacts_manifest

    def name(self) -> str:
        return 'build'

    def help(self) -> str:
        return 'Launch one or more builds'

    @functools.cached_property
    def _all_workflow_builds(self) -> Sequence[str]:
        assert self._manager
        return sorted(self._manager.get_build_names())

    @functools.cached_property
    def _all_workflow_groups(self) -> Sequence[str]:
        assert self._manager
        return sorted(self._manager.get_group_names())

    @functools.cached_property
    def _group_builds(self) -> dict[str, Sequence[str]]:
        assert self._manager
        builds = {}
        for group_name in self._all_workflow_groups:
            builds[group_name] = self._manager.get_group_build_names(group_name)
        return builds

    @functools.cached_property
    def _group_analyzers(self) -> dict[str, Sequence[str]]:
        assert self._manager
        tools = {}
        for group_name in self._all_workflow_groups:
            tools[group_name] = self._manager.get_group_analyzer_names(
                group_name
            )
        return tools

    def _print_all_builds(self) -> None:
        """Print a flat list of build and group names."""
        for build in self._all_workflow_builds:
            print(build)

        for group in self._all_workflow_groups:
            print(group)

    def _print_build_usage_help(self) -> None:
        """Print formatted usage examples for known builds."""
        build_cmd = './pw build'

        indent = 3
        indent_text = ' ' * indent
        print(_COLOR.green('All workflow builds:'))

        for b in self._all_workflow_builds:
            print(indent_text, end='')
            print(b)
        print()

        print(
            _COLOR.green('Groups:'),
        )
        for group_name in self._all_workflow_groups:
            print(_COLOR.cyan(group_name))
            step_names: list[str] = []
            step_names.extend(step for step in self._group_builds[group_name])
            step_names.extend(
                step for step in self._group_analyzers[group_name]
            )
            for b in sorted(step_names):
                print(indent_text, end='')
                print(b)

        print()

        print(_COLOR.green('Build names may use wildcards and be repeated:'))
        print(
            build_cmd,
            _COLOR.cyan('format'),
            _COLOR.cyan('\'all*\''),
        )
        print()

        print(_COLOR.green('For more help please run:'))
        print(build_cmd, _COLOR.cyan('--help'))

    def _workflow_build_argparse_type(self, arg: str) -> list[str]:
        """Argparse type function for workflow builds.

        Returns a list of matching workflow names.
        """
        all_names = list(name for name in self._all_workflow_builds)
        all_names.extend(name for name in self._all_workflow_groups)

        filtered_names = fnmatch.filter(all_names, arg)

        if not filtered_names:
            all_build_names_str = '\n'.join(sorted(all_names))
            raise argparse.ArgumentTypeError(
                f'"{arg}" does not match the name of a known build.\n\n'
                f'Valid Builds:\n\n{all_build_names_str}'
            )

        selected_builds = list(
            build
            for build in self._all_workflow_builds
            if build in filtered_names
        )
        selected_groups = list(
            group
            for group in self._all_workflow_groups
            if group in filtered_names
        )

        return selected_builds + selected_groups

    def _load_recipes(self, workflows: Sequence[str]) -> list[BuildRecipe]:
        assert self._manager

        selected_builds: list[str] = list(
            item for item in workflows if item in self._all_workflow_builds
        )

        selected_groups: list[str] = list(
            item for item in workflows if item in self._all_workflow_groups
        )

        recipes: list[BuildRecipe] = []
        recipes.extend(self._manager.create_build_recipes(selected_builds))

        for group_name in selected_groups:
            recipes.extend(self._manager.program_group(group_name))

        return recipes

    def run(self, plugin_args: Sequence[str]) -> int:
        """Handle cli args and start builds."""
        assert self._manager

        parser = argparse.ArgumentParser(
            prog='./pw build',
            description='Run pw_workflows builds',
        )
        parser.add_argument(
            '-l',
            '--list',
            help=('List all workflows.'),
            action='store_true',
        )
        parser.add_argument(
            'builds',
            metavar='BUILD_NAMES',
            nargs='*',
            help=('Names of workflow builds to run.'),
            type=self._workflow_build_argparse_type,
        )

        parser = add_project_builder_option_arguments(parser)
        args = parser.parse_args(plugin_args)
        selected_workflows = list(itertools.chain(*args.builds))

        if not selected_workflows or args.list:
            if sys.stdout.isatty():
                self._print_build_usage_help()
            else:
                self._print_all_builds()
            return 1

        _PROJECT_BUILDER_LOGGER.propagate = True

        recipes = self._load_recipes(selected_workflows)

        builder = project_builder.ProjectBuilder(
            build_recipes=recipes,
            root_logger=_PROJECT_BUILDER_LOGGER,
            jobs=args.jobs,
            banners=args.banners,
            keep_going=args.keep_going,
            colors=args.colors,
            separate_build_file_logging=args.separate_logfiles,
            root_logfile=args.logfile,
            log_level=logging.DEBUG if args.debug_logging else logging.INFO,
        )

        if builder.should_use_progress_bars():
            builder.use_stdout_proxy()

        if self._artifacts_manifest:
            builder.clean_builds()

        workers = 1
        if args.parallel:
            # If parallel is requested and parallel_workers is set to 0 run all
            # recipes in parallel. That is, use the number of recipes as the
            # worker count.
            if args.parallel_workers == 0:
                workers = len(builder)
            else:
                workers = args.parallel_workers

        result = builder.run_builds(workers)

        if self._artifacts_manifest and len(selected_workflows) == 1:
            try:
                artifacts = self._manager.collect_artifacts(
                    selected_workflows[0]
                )
                self._artifacts_manifest.write_text(
                    text_format.MessageToString(artifacts),
                )
            except TypeError:
                pass

        return result
