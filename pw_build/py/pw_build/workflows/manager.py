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
"""A library for running tools, builds, and more from a workflows.json file."""

from __future__ import annotations
from collections import defaultdict
from collections.abc import Sequence
import glob
import os
from pathlib import Path
import re
import string

from pw_build.build_recipe import BuildCommand, BuildRecipe
from pw_build.proto import build_driver_pb2, workflows_pb2
from pw_build.workflows.build_driver import BuildDriver
from pw_build.workflows.private.validator import (
    Fragment,
    Validator,
    collect_all_fragments,
)

# Regex for template/variable expansion.
_TEMPLATE_PATTERN = re.compile(r'(?:\${?)([a-zA-Z_][a-zA-Z0-9_]*)')

_BAZEL_PROJECT_ROOT_ENV_VAR = 'BUILD_WORKSPACE_DIRECTORY'
_BOOTSTRAP_PROJECT_ROOT_ENV_VAR = 'PW_PROJECT_ROOT'


def expand_action(
    action: build_driver_pb2.Action,
    env: dict[str, str | list[str]],
) -> list[str]:
    """Expands environment variables in Action arguments.

    This differs from os.path.expandvars() and string.Template in one key way:
    instances of ${FOO} that expand to a list of strings are inserted at the
    location of the variable. For example:

    Variables: {'BAR': ['c', 'd']}
    Arguments: ['a', 'b', '$BAR', 'e']

    Expanded result: ['a', 'b', 'c', 'd', 'e']

    Otherwise, the variable expansion behaves identically to
    os.path.expandvars().

    Note: This will not fail if an environment variable isn't known.
    """
    expanded_args: list[str] = []
    for arg in action.args:
        # When Python 3.11 is the minimum Python version, this can be
        # rewritten in terms of Template.get_identifiers().
        matches = _TEMPLATE_PATTERN.findall(arg)
        is_list_expansion = any(
            isinstance(env.get(match, None), list) for match in matches
        )
        if is_list_expansion:
            if len(matches) > 1:
                raise ValueError('Cannot expand pattern into multiple values')
            expanded_args.extend(env[matches[0]])
        else:
            expanded_args.append(string.Template(arg).safe_substitute(env))
    return expanded_args


class StripPrefixError(ValueError):
    pass


class NoMatchingOutputsError(ValueError):
    pass


class WorkflowsManager:
    """A class for creating build recipes from a workflows.json file.

    This class is responsible for validating Workflows configuration files,
    and converting them into executable build actions. Execution of builds
    is left to the caller to support diversity in build execution schemes.
    """

    def __init__(
        self,
        workflow_suite: workflows_pb2.WorkflowSuite,
        build_drivers: dict[str, BuildDriver],
        working_dir: Path,
        base_out_dir: Path,
        project_root: Path | None = None,
        extra_build_args: dict[str, list[str]] | None = None,
    ):
        self._workflow_suite = workflows_pb2.WorkflowSuite()
        self._workflow_suite.CopyFrom(workflow_suite)
        self._build_drivers = build_drivers
        self._fragments_by_name: dict[str, Fragment] = {}
        self._all_fragments = collect_all_fragments(self._workflow_suite)
        self._fragments_by_name = {
            fragment.name: fragment for fragment in self._all_fragments
        }
        self._shared_configs: dict[str, workflows_pb2.BuildConfig] = {
            config.name: config for config in self._workflow_suite.configs
        }
        self._shared_output_specs: dict[str, workflows_pb2.OutputGroupSpec] = {
            spec.name: spec for spec in self._workflow_suite.output_specs
        }
        if project_root is not None:
            self._project_root = project_root
        elif _BAZEL_PROJECT_ROOT_ENV_VAR in os.environ:
            self._project_root = Path(os.environ[_BAZEL_PROJECT_ROOT_ENV_VAR])
        elif _BOOTSTRAP_PROJECT_ROOT_ENV_VAR in os.environ:
            self._project_root = Path(
                os.environ[_BOOTSTRAP_PROJECT_ROOT_ENV_VAR]
            )
        else:
            raise ValueError('Cannot locate project root')
        self._base_out_dir = base_out_dir
        self._working_dir = working_dir
        self._extra_build_args = extra_build_args or {}
        Validator(workflow_suite, build_drivers).validate()
        self._init_workflow_defaults()

    def _init_workflow_defaults(self):
        for build in self._workflow_suite.builds:
            if not build.rerun_shortcut:
                build.rerun_shortcut = f'build {build.name}'
        for tool in self._workflow_suite.tools:
            if not tool.rerun_shortcut:
                tool.rerun_shortcut = f'check {tool.name}'

    def get_build_names(self) -> Sequence[str]:
        return list(
            build.name
            for build in self._fragments_by_name.values()
            if isinstance(build, workflows_pb2.Build)
        )

    def get_group_names(self) -> Sequence[str]:
        return list(
            group.name
            for group in self._fragments_by_name.values()
            if isinstance(group, workflows_pb2.TaskGroup)
        )

    def get_group_build_names(self, group_name: str) -> Sequence[str]:
        builds = []
        group = self._get_group_fragment(group_name)

        for build_name in group.builds:
            build = self._fragments_by_name[build_name]
            if isinstance(build, workflows_pb2.Build):
                builds.append(build.name)
        return builds

    def get_group_analyzer_names(self, group_name: str) -> Sequence[str]:
        group = self._get_group_fragment(group_name)

        tools = []
        for tool_name in group.analyzers:
            tool = self._fragments_by_name[tool_name]
            if isinstance(tool, workflows_pb2.Tool):
                tools.append(tool.name)
        return tools

    def _get_group_fragment(self, group_name: str) -> workflows_pb2.TaskGroup:
        group = self._fragments_by_name.get(group_name, None)
        if not isinstance(group, workflows_pb2.TaskGroup):
            raise TypeError(f'{group_name} is not a group.')
        return group

    def _get_build_config(
        self,
        fragment: Fragment,
    ) -> workflows_pb2.BuildConfig:
        if not isinstance(fragment, (workflows_pb2.Tool, workflows_pb2.Build)):
            raise TypeError(
                'Cannot get a build config from something that is not a build '
                f'or tool: {fragment}'
            )
        if fragment.WhichOneof('config') == 'use_config':
            return self._shared_configs[fragment.use_config]
        return fragment.build_config

    def _build_dir(self, config: workflows_pb2.BuildConfig) -> Path:
        return self._base_out_dir / config.name

    def _action_working_dir(
        self,
        action: build_driver_pb2.Action,
        build_dir: Path,
    ) -> Path:
        if (
            action.run_from
            == build_driver_pb2.Action.InvocationLocation.PROJECT_ROOT
        ):
            return self._project_root
        if (
            action.run_from
            == build_driver_pb2.Action.InvocationLocation.INVOKER_CWD
        ):
            return self._working_dir
        if (
            action.run_from
            == build_driver_pb2.Action.InvocationLocation.PROJECT_SUBDIR
        ):
            return self._project_root / action.project_working_subdir
        return build_dir

    def _postprocess_job_response(
        self,
        job_response: build_driver_pb2.JobResponse,
        build_dir: Path,
        forwarded_args: Sequence[str],
    ) -> None:
        """Updates the incoming JobResponse to expand all variables."""
        for action in job_response.actions:
            working_dir = self._action_working_dir(
                action,
                build_dir,
            )
            # TODO: https://pwbug.dev/428715231 - Define these elsewhere
            # in a way that makes documentation generation of this list
            # scalable/maintainable.
            substitutions: dict[str, str | list[str]] = {
                'FORWARDED_LAUNCH_ARGS': list(forwarded_args),
                'BUILD_ROOT': str(
                    os.path.relpath(build_dir.resolve(), working_dir.resolve())
                ),
                'PROJECT_ROOT': str(
                    os.path.relpath(
                        self._project_root.resolve(),
                        working_dir.resolve(),
                    )
                ),
                'INVOKER_DIR': str(
                    os.path.relpath(
                        self._working_dir.resolve(),
                        working_dir.resolve(),
                    )
                ),
            }
            expanded_args = expand_action(action, substitutions)
            action.ClearField('args')
            action.args.extend(expanded_args)

    def create_build_recipes(
        self,
        fragment_names: list[str],
        forwarded_args: Sequence[str] | None = None,
    ) -> list[BuildRecipe]:
        """Creates build recipes for fragments by name."""

        fragments = []
        for name in fragment_names:
            fragment = self._fragments_by_name.get(name, None)
            if not fragment:
                raise TypeError(
                    f'{name} is not a known configuration fragment.'
                )
            fragments.append(fragment)

        return self._create_build_recipes(fragments, forwarded_args)

    def _create_build_recipes(
        self,
        fragments: list[Fragment],
        forwarded_args: Sequence[str] | None = None,
    ) -> list[BuildRecipe]:
        """Creates build recipes for a series of configuration fragments."""
        if forwarded_args is None:
            forwarded_args = []

        build_driver_requests: dict[
            str, build_driver_pb2.BuildDriverRequest
        ] = defaultdict(build_driver_pb2.BuildDriverRequest)
        for fragment in fragments:
            if isinstance(fragment, workflows_pb2.Tool):
                config = self._get_build_config(fragment)
                job_request = build_driver_pb2.JobRequest(
                    tool=fragment,
                )
                build_driver_requests[config.build_type].jobs.append(
                    job_request
                )
            elif isinstance(fragment, workflows_pb2.Build):
                config = self._get_build_config(fragment)
                job_request = build_driver_pb2.JobRequest(
                    build=fragment,
                )
                build_driver_requests[config.build_type].jobs.append(
                    job_request
                )
            else:
                raise ValueError(
                    'Tried to create a build from an unsupported fragment '
                    f'type: {fragment}'
                )

        build_recipes = []
        for build_type, request in build_driver_requests.items():
            driver = self._build_drivers[build_type]
            response = driver.generate_jobs(
                self._prepare_driver_request(request, sanitize=True)
            )
            for job_request, job_response in zip(request.jobs, response.jobs):
                fragment = (
                    job_request.tool
                    if job_request.WhichOneof('type') == 'tool'
                    else job_request.build
                )
                request_config = self._get_build_config(
                    self._fragments_by_name[fragment.name]
                )
                build_dir = self._build_dir(request_config)
                steps = []
                self._postprocess_job_response(
                    job_response=job_response,
                    build_dir=build_dir,
                    forwarded_args=forwarded_args,
                )
                for action in job_response.actions:
                    working_dir = self._action_working_dir(
                        action,
                        build_dir,
                    )
                    targets = (
                        [fragment.target]
                        if hasattr(fragment, 'target')
                        else list(fragment.targets)
                    )
                    steps.append(
                        BuildCommand(
                            command=[
                                action.executable,
                                *action.args,
                            ],
                            working_dir=working_dir,
                            targets=targets,
                        )
                    )

                clean_globs: list[str] = []
                for output_spec in fragment.output_spec:
                    clean_globs.extend(output_spec.glob_patterns)

                build_recipes.append(
                    BuildRecipe(
                        build_dir=build_dir,
                        title=fragment.rerun_shortcut,
                        steps=steps,
                        clean_globs=clean_globs,
                    )
                )
        return build_recipes

    def _prepare_driver_request(
        self,
        request: build_driver_pb2.BuildDriverRequest,
        sanitize: bool,
    ) -> build_driver_pb2.BuildDriverRequest:
        """Prepares a build_driver_pb2.BuildDriverRequest.

        This helper takes an incoming build_driver_pb2.BuildDriverRequest and
        prepares it so that a build driver has all the information it needs.

        * Copies the incoming request.
        * Expands referenced shared build configs.
        * Removes info that build drivers shouldn't see/use from a request.

        Returns:
            A properly prepared copy of the incoming
                build_driver_pb2.BuildDriverRequest.
        """
        final_request = build_driver_pb2.BuildDriverRequest()
        for job in request.jobs:
            prepared_job = build_driver_pb2.JobRequest()
            prepared_job.CopyFrom(job)
            fragment = (
                prepared_job.build
                if prepared_job.WhichOneof('type') == 'build'
                else prepared_job.tool
            )
            if fragment.WhichOneof('config') == 'use_config':
                config = self._get_build_config(fragment)
                fragment.ClearField('use_config')
                fragment.build_config.CopyFrom(config)
            if hasattr(fragment, "use_output"):
                for spec_name in fragment.use_output:
                    fragment.output_spec.append(
                        self._shared_output_specs[spec_name],
                    )
                fragment.ClearField('use_output')

            build_type = fragment.build_config.build_type
            if build_type in self._extra_build_args:
                fragment.build_config.args.extend(
                    self._extra_build_args[build_type]
                )

            if sanitize:
                fragment.ClearField('name')
                fragment.ClearField('description')
                fragment.ClearField('rerun_shortcut')
                fragment.build_config.ClearField('name')
                fragment.build_config.ClearField('description')
            final_request.jobs.append(prepared_job)
        return final_request

    def program_tool(
        self,
        tool_name: str,
        forwarded_arguments: Sequence[str],
        as_analyzer: bool = False,
    ) -> list[BuildRecipe]:
        """Generates build recipes for a workflows_pb2.Tool by name.

        Args:
            tool_name: The name of the Tool to launch.
            forwarded_arguments: Arguments to forward when launching the
                underlying tool.
            as_analyzer: If true, launches the requested tool in analyzer
                mode (if supported). This tells the tool to not modify
                any in-tree sources.

        Returns:
            A list of BuildRecipes that fulfill this request.
        """
        tool = self._fragments_by_name.get(tool_name, None)
        if not isinstance(tool, workflows_pb2.Tool):
            raise TypeError(f'{tool_name} is not a tool.')
        if as_analyzer:
            if (
                tool.type != workflows_pb2.Tool.Type.ANALYZER
                and not tool.analyzer_friendly_args
            ):
                raise TypeError(
                    f'Tool `{tool_name}` cannot be run as an analyzer. '
                    'Either add `analyzer_friendly_args` or ensure the tool '
                    'is safe to run as an analyzer'
                )
            forwarded_arguments = [
                *tool.analyzer_friendly_args,
                *forwarded_arguments,
            ]
        return self._create_build_recipes([tool], forwarded_arguments)

    def program_build(self, build_name: str) -> list[BuildRecipe]:
        """Generates build recipes for a workflows_pb2.Build by name.

        Args:
            build_name: The name of the Build to launch.

        Returns:
            A list of BuildRecipes that fulfill this request.
        """
        build = self._fragments_by_name.get(build_name, None)
        if not isinstance(build, workflows_pb2.Build):
            raise TypeError(f'{build_name} is not a build.')
        return self._create_build_recipes([build])

    def program_group(self, group_name: str) -> list[BuildRecipe]:
        """Generates build recipes for a workflows_pb2.TaskGroup by name.

        Args:
            group_name: The name of the Group to launch.

        Returns:
            A list of BuildRecipes that fulfill this request.
        """
        group = self._get_group_fragment(group_name)

        recipes: list[BuildRecipe] = []

        for sub_group_name in group.groups:
            recipes.extend(self.program_group(sub_group_name))

        builds = [
            self._fragments_by_name[build_name] for build_name in group.builds
        ]
        recipes.extend(self._create_build_recipes(builds))

        for tool_name in group.analyzers:
            tool = self._fragments_by_name[tool_name]
            if not isinstance(tool, workflows_pb2.Tool):
                raise TypeError(f'{tool_name} is not a tool.')
            recipes.extend(
                self._create_build_recipes(
                    [tool],
                    list(tool.analyzer_friendly_args),
                )
            )

        return recipes

    def program_by_name(self, name: str) -> list[BuildRecipe]:
        """Generates build recipes for a group, build, or analyzer by name.

        Prefer this method when strict checking of the requested Workflows
        configuration type isn't required. For example, if you just want to
        execute 'format' and don't know

        Args:
            name: The name of the buildable unit to program.

        Returns:
            A list of BuildRecipes that fulfill this request.
        """
        fragment = self._fragments_by_name.get(name, None)
        if isinstance(fragment, workflows_pb2.TaskGroup):
            return self.program_group(name)
        if isinstance(fragment, workflows_pb2.Build):
            return self.program_build(name)
        if isinstance(fragment, workflows_pb2.Tool):
            # When arbitrarily launching a tool, launch as an analyzer
            # so it doesn't unintentionally modify the source tree.
            return self.program_tool(
                name,
                forwarded_arguments=tuple(),
                as_analyzer=True,
            )

        raise TypeError(f'{name} is not a buildable unit')

    def get_unified_driver_request(
        self, buildable_names: Sequence[str], sanitize: bool = False
    ) -> build_driver_pb2.BuildDriverRequest:
        """Creates a BuildDriverRequest for a series of configuration fragments.

        This produces a single, unified view of all the builds and tool
        invocations that will be launched for the given requested builds/tools.

        Args:
            buildable_names: A list of build, tool, or group names.
            sanitize: If true, strips fields that build drivers will not see.

        Returns:
            A BuildDriverRequest message containing jobs for all the resolved
            fragments.
        """
        resolved_fragments: list[Fragment] = []
        for name in buildable_names:
            fragment = self._fragments_by_name.get(name)
            if isinstance(fragment, workflows_pb2.TaskGroup):
                for build_name in fragment.builds:
                    resolved_fragments.append(
                        self._fragments_by_name[build_name]
                    )
                for analyzer_name in fragment.analyzers:
                    resolved_fragments.append(
                        self._fragments_by_name[analyzer_name]
                    )
            elif isinstance(
                fragment, (workflows_pb2.Build, workflows_pb2.Tool)
            ):
                resolved_fragments.append(fragment)
            else:
                raise AssertionError(
                    f'No build, tool, or group named `{name}` found in this '
                    'workflow configuration'
                )

        request = build_driver_pb2.BuildDriverRequest()
        for fragment in resolved_fragments:
            if isinstance(fragment, workflows_pb2.Build):
                request.jobs.append(build_driver_pb2.JobRequest(build=fragment))
            elif isinstance(fragment, workflows_pb2.Tool):
                request.jobs.append(build_driver_pb2.JobRequest(tool=fragment))
            else:
                raise TypeError(
                    f'Internal error: `{fragment.name}` is an unexpected type'
                )
        return self._prepare_driver_request(request, sanitize=sanitize)

    def collect_artifacts(
        self,
        fragment_name: str,
    ) -> workflows_pb2.BuildArtifacts:
        """Collect matching artifacts."""
        result = workflows_pb2.BuildArtifacts()

        fragment = self._fragments_by_name[fragment_name]

        if not hasattr(fragment, 'output_spec'):
            raise TypeError(f'{fragment} does not have an output_spec member.')

        config = self._get_build_config(fragment)

        output_root = self._build_dir(config)
        result.output_root = str(output_root)

        for spec in fragment.output_spec:
            outputs: list[str] = []
            for pattern in spec.glob_patterns:
                for match in glob.iglob(
                    pattern,
                    root_dir=output_root,
                    recursive=True,
                ):
                    match = match.replace(os.sep, '/')
                    outputs.append(match)

                    if spec.strip_prefix:
                        strip_prefix = spec.strip_prefix.replace(os.sep, '/')
                        if not match.startswith(strip_prefix):
                            raise StripPrefixError(
                                f'{match} does not match strip_prefix '
                                f'({strip_prefix})'
                            )

            if not outputs and not spec.allow_empty:
                raise NoMatchingOutputsError(
                    f'no matching outputs for output group spec {spec.name}',
                )

            if outputs:
                group = workflows_pb2.OutputGroup()
                group.matching_files.extend(outputs)
                group.strip_prefix = spec.strip_prefix
                group.add_prefix = spec.add_prefix
                result.output_groups.append(group)

        return result
