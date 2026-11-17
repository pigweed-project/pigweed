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
"""Validation logic for Workflows configurations.

Any rules, preconditions, or relationships that are not strictly
enforced by the Workflows proto schema must be validated here.

Some high-level examples:

* `name` fields in the Workflows configurations are used as identifiers,
  and therefore must be unique.
* To prevent duplication and make authoring configurations, many components
  in a Workflows configuration may reference others by `name`. Since these are
  strings, checks must be done to ensure the referenced pieces both exist and
  are of the expected type.

Some pieces of the Workflows tooling are guarded by this validation logic to
unify preconditions. This, for example, allows all pieces of the Workflows
libraries to assume every referenceable piece of a configuration has a name that
is both populated an unique, which greatly cuts down on code duplication and
inline `if` statements.
"""

from collections.abc import Callable

from google.protobuf import text_format
from pw_build.proto import workflows_pb2
from pw_build.workflows.build_driver import BuildDriver
from pw_build.workflows.private.mixins import merge_build_configs


class ValidationError(Exception):
    """A Workflows configuration error."""


Fragment = (
    workflows_pb2.BuildConfig
    | workflows_pb2.TaskGroup
    | workflows_pb2.Tool
    | workflows_pb2.Build
    | workflows_pb2.OutputGroupSpec
)


def collect_all_fragments(
    workflow_suite: workflows_pb2.WorkflowSuite,
) -> list[Fragment]:
    """Collect all Fragments into one list."""
    all_fragments: list[Fragment] = []
    all_fragments.extend(workflow_suite.output_specs)
    all_fragments.extend(workflow_suite.configs)
    all_fragments.extend(workflow_suite.builds)
    all_fragments.extend(
        [
            build.build_config
            for build in workflow_suite.builds
            if build.WhichOneof('config') == 'build_config'
        ]
    )
    all_fragments.extend(workflow_suite.tools)
    all_fragments.extend(
        [
            tool.build_config
            for tool in workflow_suite.tools
            if tool.WhichOneof('config') == 'build_config'
        ]
    )
    all_fragments.extend(workflow_suite.groups)

    output_specs: list[workflows_pb2.OutputGroupSpec] = []
    for build in workflow_suite.builds:
        output_specs.extend(build.output_spec)
    for tool in workflow_suite.tools:
        output_specs.extend(tool.output_spec)
    all_fragments.extend(output_specs)

    return all_fragments


class Validator:
    """A class for validating workflows configurations."""

    def __init__(
        self,
        workflow_suite: workflows_pb2.WorkflowSuite,
        build_drivers: dict[str, BuildDriver],
    ):
        self._workflow_suite = workflow_suite
        self._build_drivers = build_drivers
        self._fragments_by_name: dict[str, Fragment] = {}
        self._all_fragments = collect_all_fragments(self._workflow_suite)
        self._fragments_by_name = {
            fragment.name: fragment for fragment in self._all_fragments
        }
        self._shared_config_names = {
            config.name for config in self._workflow_suite.configs
        }
        self._shared_output_names = {
            output.name for output in self._workflow_suite.output_specs
        }
        self._shared_configs = {
            config.name: config for config in self._workflow_suite.configs
        }
        # Build directories are named after configs, so merged config names
        # must not match any shared or inline config name.
        self._config_names = {
            fragment.name
            for fragment in self._all_fragments
            if isinstance(fragment, workflows_pb2.BuildConfig)
        }

    def validate(self) -> None:
        """Runs all checks on the loaded Workflows config.

        This runs all check_*() methods on this class, efficiently
        grouping by categories.

        """
        all_fragment_checks: list[Callable[[Fragment], None]] = []
        config_fragment_checks: list[
            Callable[[workflows_pb2.BuildConfig], None]
        ] = []
        group_fragment_checks: list[
            Callable[[workflows_pb2.TaskGroup], None]
        ] = []
        tool_fragment_checks: list[Callable[[workflows_pb2.Tool], None]] = []
        build_fragment_checks: list[Callable[[workflows_pb2.Build], None]] = []
        output_fragment_checks: list[
            Callable[[workflows_pb2.OutputGroupSpec], None]
        ] = []
        for attr in dir(self):
            if attr.startswith('check_fragment_'):
                all_fragment_checks.append(getattr(self, attr))
            elif attr.startswith('check_config_'):
                config_fragment_checks.append(getattr(self, attr))
            elif attr.startswith('check_build_'):
                build_fragment_checks.append(getattr(self, attr))
            elif attr.startswith('check_tool_'):
                tool_fragment_checks.append(getattr(self, attr))
            elif attr.startswith('check_group_'):
                group_fragment_checks.append(getattr(self, attr))
            elif attr.startswith('check_output_'):
                output_fragment_checks.append(getattr(self, attr))
        for fragment in self._all_fragments:
            for check in all_fragment_checks:
                check(fragment)
            if isinstance(fragment, workflows_pb2.Build):
                for build_check in build_fragment_checks:
                    build_check(fragment)
            elif isinstance(fragment, workflows_pb2.BuildConfig):
                for config_check in config_fragment_checks:
                    config_check(fragment)
            elif isinstance(fragment, workflows_pb2.Tool):
                for tool_check in tool_fragment_checks:
                    tool_check(fragment)
            elif isinstance(fragment, workflows_pb2.TaskGroup):
                for group_check in group_fragment_checks:
                    group_check(fragment)
            elif isinstance(fragment, workflows_pb2.OutputGroupSpec):
                for output_check in output_fragment_checks:
                    output_check(fragment)
            else:
                raise ValidationError(f'Unknown fragment type: {fragment}')
        self._check_merged_names_unique()

    def check_fragment_has_unique_name(self, fragment: Fragment) -> None:
        existing_fragment = self._fragments_by_name[fragment.name]
        if fragment is not existing_fragment:
            first_match = text_format.MessageToString(
                existing_fragment, as_one_line=True
            )
            second_match = text_format.MessageToString(
                fragment, as_one_line=True
            )
            raise ValidationError(
                f'The name `{fragment.name}` is shared by the following '
                'configuration elements:\n'
                f'[{first_match}] and '
                f'[{second_match}]'
            )

    @staticmethod
    def check_fragment_has_name(fragment: Fragment) -> None:
        if not fragment.name:
            raise ValidationError(
                'The following configuration element is unnamed:\n'
                + str(fragment)
            )

    def check_config_references_real_build_type(
        self, config: workflows_pb2.BuildConfig
    ) -> None:
        if config.mixin and not config.build_type:
            return
        if config.build_type not in self._build_drivers:
            raise ValidationError(
                f'BuildConfig `{config.name}` references unknown build type '
                f'`{config.build_type}`'
            )

    @staticmethod
    def check_config_mixin_driver_options_have_build_type(
        config: workflows_pb2.BuildConfig,
    ) -> None:
        # Without a build_type, nothing ties the options' message type to the
        # driver of the base config the mixin is applied to.
        if config.mixin and config.driver_options.type_url:
            if not config.build_type:
                raise ValidationError(
                    f'Mixin `{config.name}` sets `driver_options`, so it must '
                    'also set `build_type`'
                )

    def check_build_references_real_config(
        self, build: workflows_pb2.Build
    ) -> None:
        if (
            build.use_config
            and build.use_config not in self._shared_config_names
        ):
            raise ValidationError(
                f'Build `{build.name}` references missing build config '
                f'`{build.use_config}`'
            )

    def check_build_references_real_output(
        self, build: workflows_pb2.Build
    ) -> None:
        for output in build.use_output:
            if output not in self._shared_output_names:
                raise ValidationError(
                    f'Build `{build.name}` references missing output spec '
                    f'`{output}`'
                )

    @staticmethod
    def check_build_has_config(build: workflows_pb2.Build) -> None:
        which = build.WhichOneof('config')
        if which != 'use_config' and which != 'build_config':
            raise ValidationError(
                f'Build `{build.name}` has no associated configuration'
            )

    @staticmethod
    def check_build_has_targets(build: workflows_pb2.Build) -> None:
        if not build.targets:
            raise ValidationError(f'Build `{build.name}` has no targets.')

    def check_tool_references_real_config(
        self, tool: workflows_pb2.Tool
    ) -> None:
        if tool.use_config and tool.use_config not in self._shared_config_names:
            raise ValidationError(
                f'Tool `{tool.name}` references missing build config '
                f'`{tool.use_config}`'
            )

    def check_tool_references_real_output(
        self, tool: workflows_pb2.Tool
    ) -> None:
        for output in tool.use_output:
            if output not in self._shared_output_names:
                raise ValidationError(
                    f'Tool `{tool.name}` references missing output spec '
                    f'`{output}`'
                )

    @staticmethod
    def check_tool_has_config(tool: workflows_pb2.Tool) -> None:
        which = tool.WhichOneof('config')
        if which != 'use_config' and which != 'build_config':
            raise ValidationError(
                f'Tool `{tool.name}` has no associated configuration'
            )

    @staticmethod
    def check_tool_rerun_shortcut_if_hybrid(tool: workflows_pb2.Tool) -> None:
        if not tool.rerun_shortcut:
            return
        if (
            tool.type != workflows_pb2.Tool.Type.GENERAL
            or not tool.analyzer_friendly_args
        ):
            raise ValidationError(
                f'Tool `{tool.name}` cannot remap its `rerun_shortcut` because '
                'it is not a GENERAL tool with `analyzer_friendly_args`. The '
                'only way it may be invoked is by its name.'
            )

    @staticmethod
    def check_tool_has_target(tool: workflows_pb2.Tool) -> None:
        if not tool.target:
            raise ValidationError(f'Tool `{tool.name}` has no target.')

    def _base_config(
        self, fragment: workflows_pb2.Build | workflows_pb2.Tool
    ) -> workflows_pb2.BuildConfig | None:
        """Returns the base config of a build or tool, or None if missing."""
        which = fragment.WhichOneof('config')
        if which == 'use_config':
            return self._shared_configs.get(fragment.use_config)
        if which == 'build_config':
            return fragment.build_config
        return None

    def _merged_config(
        self, fragment: workflows_pb2.Build | workflows_pb2.Tool
    ) -> workflows_pb2.BuildConfig | None:
        """Returns the merged config of a build or tool that lists mixins.

        Returns None if the fragment lists no mixins, or if its base config is
        missing (that case is reported by other checks). Call this only after
        _check_mixins() has passed for the fragment.
        """
        base = self._base_config(fragment)
        if base is None or not fragment.use_config_mixins:
            return None
        return merge_build_configs(
            base,
            [self._shared_configs[name] for name in fragment.use_config_mixins],
        )

    def _check_mixins(
        self, kind: str, fragment: workflows_pb2.Build | workflows_pb2.Tool
    ) -> None:
        """Checks the mixin rules for one build or tool, in order."""
        name = fragment.name
        base = self._base_config(fragment)
        if base is not None and base.mixin:
            if fragment.WhichOneof('config') == 'use_config':
                raise ValidationError(
                    f'{kind} `{name}` cannot use mixin `{base.name}` as its '
                    '`use_config`'
                )
            raise ValidationError(
                f'{kind} `{name}` has an inline `build_config` marked as a '
                'mixin; mixins must be shared configs'
            )

        seen: set[str] = set()
        for mixin_name in fragment.use_config_mixins:
            mixin = self._shared_configs.get(mixin_name)
            if mixin is None:
                raise ValidationError(
                    f'{kind} `{name}` references missing mixin `{mixin_name}`'
                )
            if not mixin.mixin:
                raise ValidationError(
                    f'{kind} `{name}` lists `{mixin_name}` in '
                    f'`use_config_mixins`, but `{mixin_name}` is not a mixin'
                )
            if mixin_name in seen:
                raise ValidationError(
                    f'{kind} `{name}` lists mixin `{mixin_name}` more than once'
                )
            seen.add(mixin_name)

        try:
            merged = self._merged_config(fragment)
        except ValueError as e:
            raise ValidationError(f'{kind} `{name}`: {e}') from e
        if merged is not None and merged.name in self._config_names:
            raise ValidationError(
                f'{kind} `{name}`: merged config name `{merged.name}` collides '
                'with an existing build config name'
            )

    def check_build_mixins(self, build: workflows_pb2.Build) -> None:
        self._check_mixins('Build', build)

    def check_tool_mixins(self, tool: workflows_pb2.Tool) -> None:
        self._check_mixins('Tool', tool)

    def _check_merged_names_unique(self) -> None:
        """Ensures different mixin combinations get different names.

        This looks across all builds and tools, so validate() calls it once
        after the per-fragment checks have passed.
        """
        seen: dict[str, tuple[str, workflows_pb2.BuildConfig]] = {}
        fragments: list[workflows_pb2.Build | workflows_pb2.Tool] = [
            *self._workflow_suite.builds,
            *self._workflow_suite.tools,
        ]
        for fragment in fragments:
            merged = self._merged_config(fragment)
            if merged is None:
                continue
            if merged.name not in seen:
                seen[merged.name] = (fragment.name, merged)
                continue
            other_name, other = seen[merged.name]
            if other != merged:
                raise ValidationError(
                    f'Merged config name `{merged.name}` collides: '
                    f'`{other_name}` and `{fragment.name}` merge different '
                    'configs into it'
                )

    def check_group_builds_exist(self, group: workflows_pb2.TaskGroup) -> None:
        for build_name in group.builds:
            if build_name not in self._fragments_by_name:
                raise ValidationError(
                    f'TaskGroup `{group.name}` references missing build '
                    f'`{build_name}`'
                )

    def check_group_analyzers_exist(
        self, group: workflows_pb2.TaskGroup
    ) -> None:
        for analyzer_name in group.analyzers:
            if analyzer_name not in self._fragments_by_name:
                raise ValidationError(
                    f'TaskGroup `{group.name}` references missing analyzer '
                    f'`{analyzer_name}`'
                )
            fragment = self._fragments_by_name[analyzer_name]
            if not isinstance(fragment, workflows_pb2.Tool):
                raise ValidationError(
                    f'TaskGroup `{group.name}` references analyzer '
                    f'`{analyzer_name}` which is not a tool.'
                )
            if (
                fragment.type != workflows_pb2.Tool.Type.ANALYZER
                and not fragment.analyzer_friendly_args
            ):
                raise ValidationError(
                    f'TaskGroup `{group.name}` references analyzer '
                    f'`{analyzer_name}` which is not of type ANALYZER and '
                    'has no analyzer_friendly_args.'
                )

    def check_group_groups_exist(self, group: workflows_pb2.TaskGroup) -> None:
        for group_name in group.groups:
            if group_name not in self._fragments_by_name:
                raise ValidationError(
                    f'TaskGroup `{group.name}` references missing group '
                    f'`{group_name}`'
                )
            fragment = self._fragments_by_name[group_name]
            if not isinstance(fragment, workflows_pb2.TaskGroup):
                raise ValidationError(
                    f'TaskGroup `{group.name}` references group '
                    f'`{group_name}` which is not a TaskGroup.'
                )

    def check_group_has_no_cycles(self, group: workflows_pb2.TaskGroup) -> None:
        def has_cycle(current_name: str, path: set[str]) -> bool:
            if current_name in path:
                return True

            fragment = self._fragments_by_name.get(current_name)
            if not isinstance(fragment, workflows_pb2.TaskGroup):
                return False

            new_path = path | {current_name}
            for sub_group_name in fragment.groups:
                if has_cycle(sub_group_name, new_path):
                    return True
            return False

        if has_cycle(group.name, set()):
            raise ValidationError(
                f'TaskGroup `{group.name}` has a cyclic dependency.'
            )

    @staticmethod
    def check_output_has_patterns(
        output: workflows_pb2.OutputGroupSpec,
    ) -> None:
        if not output.glob_patterns:
            raise ValidationError(
                f'Output group spec `{output.name}` has no glob_patterns.',
            )
