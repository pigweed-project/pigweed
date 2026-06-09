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
"""Bazel driver for workflows.

This tool converts an incoming Bazel BuildDriverRequest as ProtoJSON
into a BuildDriverResponse that contains a sequence of actions that make up
a bazel build.

Example:

$ bazel run //pw_build/py:bazel_build_driver -- \
    --request=$PWD/pw_build/test_data/bazel_build_driver_request.json

"""

from collections.abc import Sequence
import sys

from pw_build.proto import (
    build_driver_pb2,
    workflows_pb2,
    pigweed_build_driver_pb2,
)
from pw_build.workflows.build_driver import BuildDriver

# These lists of arguments come from `bazel help startup_options`. We could
# parse them out at runtime, but for now we'll just stick with a simpler
# hard-coded list so we don't have to make calls to Bazel yet.

# A list of all Bazel startup flags that take a value.
_STARTUP_FLAGS_WITH_VALUES = {
    '--bazelrc',
    '--connect_timeout_secs',
    '--digest_function',
    '--experimental_cgroup_parent',
    '--failure_detail_out',
    '--io_nice_level',
    '--local_startup_timeout_secs',
    '--macos_qos_class',
    '--max_idle_secs',
    '--output_base',
    '--output_user_root',
    '--server_jvm_out',
    '--host_jvm_args',
    '--server_javabase',
}

# A list of all Bazel startup flags that are boolean and can be negated with
# --no.
_STARTUP_FLAGS_BOOLEAN = {
    '--autodetect_server_javabase',
    '--batch',
    '--batch_cpu_scheduling',
    '--block_for_lock',
    '--client_debug',
    '--experimental_run_in_user_cgroup',
    '--home_rc',
    '--idle_server_tasks',
    '--ignore_all_rc_files',
    '--preemptible',
    '--quiet',
    '--shutdown_on_low_sys_mem',
    '--system_rc',
    '--unlimit_coredumps',
    '--watchfs',
    '--windows_enable_symlinks',
    '--workspace_rc',
    '--host_jvm_debug',
}
_STARTUP_FLAGS_BOOLEAN.update(
    ['--no' + arg.removeprefix('--') for arg in _STARTUP_FLAGS_BOOLEAN]
)


class BazelBuildDriver(BuildDriver):
    """A Workflows build driver for Bazel builds."""

    def generate_action_sequence(
        self, job: build_driver_pb2.JobRequest
    ) -> build_driver_pb2.JobResponse:
        if job.WhichOneof('type') == 'tool':
            return self.generate_action_sequence_for_tool(job.tool)
        if job.WhichOneof('type') == 'build':
            return self.generate_action_sequence_for_build(job.build)

        # This is a nop.
        return build_driver_pb2.JobResponse()

    @staticmethod
    def extra_descriptors():
        return [pigweed_build_driver_pb2.DESCRIPTOR]

    def generate_action_sequence_for_build(
        self, build: workflows_pb2.Build
    ) -> build_driver_pb2.JobResponse:
        """Generate a sequence of actions to satisfy a Bazel build."""
        actions = build_driver_pb2.JobResponse()

        startup_args, other_args = self._split_startup_args(
            build.build_config.args
        )

        common_args = [
            '--symlink_prefix=${BUILD_ROOT}/bazel-',
            *other_args,
        ]

        driver_options = self.unpack_driver_options(
            pigweed_build_driver_pb2.BazelDriverOptions,
            build.build_config.driver_options,
        )

        run_from = build_driver_pb2.Action.InvocationLocation.INVOKER_CWD
        env = build.build_config.env
        project_working_subdir = ''
        if driver_options.project_working_subdir:
            project_working_subdir = driver_options.project_working_subdir
            run_from = build_driver_pb2.Action.InvocationLocation.PROJECT_SUBDIR

        actions.actions.append(
            self._canonicalize_args(build.build_config, project_working_subdir)
        )
        actions.actions.append(
            build_driver_pb2.Action(
                executable='bazelisk',
                args=[
                    *startup_args,
                    'build',
                    *common_args,
                    "--",
                    *build.targets,
                ],
                env=env,
                run_from=run_from,
                project_working_subdir=project_working_subdir,
            )
        )
        if not driver_options.no_test:
            actions.actions.append(
                build_driver_pb2.Action(
                    executable='bazelisk',
                    args=[
                        *startup_args,
                        'test',
                        *common_args,
                        "--",
                        *build.targets,
                    ],
                    env=env,
                    run_from=run_from,
                    project_working_subdir=project_working_subdir,
                )
            )
        return actions

    def generate_action_sequence_for_tool(
        self, tool: workflows_pb2.Tool
    ) -> build_driver_pb2.JobResponse:
        """Generate a sequence of actions to run a Bazel-hosted tool."""
        actions = build_driver_pb2.JobResponse()
        startup_args, other_args = self._split_startup_args(
            tool.build_config.args
        )

        driver_options = self.unpack_driver_options(
            pigweed_build_driver_pb2.BazelDriverOptions,
            tool.build_config.driver_options,
        )

        run_from = build_driver_pb2.Action.InvocationLocation.INVOKER_CWD
        project_working_subdir = ''
        if driver_options.project_working_subdir:
            project_working_subdir = driver_options.project_working_subdir
            run_from = build_driver_pb2.Action.InvocationLocation.PROJECT_SUBDIR

        actions.actions.append(
            self._canonicalize_args(tool.build_config, project_working_subdir)
        )

        bazel_run_action = build_driver_pb2.Action(
            executable='bazelisk',
            args=[
                *startup_args,
                'run',
                '--ui_event_filters=FATAL,ERROR,PROGRESS',
                '--experimental_convenience_symlinks=ignore',
                *other_args,
                tool.target,
                '--',
                '${FORWARDED_LAUNCH_ARGS}',
            ],
            env=tool.build_config.env,
            run_from=run_from,
            project_working_subdir=project_working_subdir,
        )
        actions.actions.append(bazel_run_action)
        return actions

    @staticmethod
    def _split_startup_args(
        args: Sequence[str],
    ) -> tuple[list[str], list[str]]:
        """Splits a list of Bazel arguments into startup and other args."""
        startup_args: list[str] = []
        other_args: list[str] = []
        arg_iter = iter(args)
        for arg in arg_iter:
            # Handle flags that take a value.
            if arg in _STARTUP_FLAGS_WITH_VALUES:
                startup_args.append(arg)
                try:
                    startup_args.append(next(arg_iter))
                except StopIteration:
                    # This is an error, but we'll let Bazel complain.
                    pass
                continue

            # Handle flags that take a value with an equals sign.
            if arg.startswith('--') and '=' in arg:
                if arg.split('=', 1)[0] in _STARTUP_FLAGS_WITH_VALUES:
                    startup_args.append(arg)
                    continue

            # Handle boolean flags (e.g. --batch / --nobatch).
            if arg in _STARTUP_FLAGS_BOOLEAN:
                startup_args.append(arg)
                continue

            other_args.append(arg)

        return startup_args, other_args

    @staticmethod
    def _canonicalize_args(
        build_config: workflows_pb2.BuildConfig,
        project_working_subdir: str = '',
    ) -> build_driver_pb2.Action:
        """Ensures that Bazel configs don't include target patterns."""
        startup_args, other_args = BazelBuildDriver._split_startup_args(
            build_config.args
        )
        run_from = build_driver_pb2.Action.InvocationLocation.INVOKER_CWD
        if project_working_subdir:
            run_from = build_driver_pb2.Action.InvocationLocation.PROJECT_SUBDIR

        return build_driver_pb2.Action(
            executable='bazelisk',
            args=[*startup_args, 'canonicalize-flags', '--', *other_args],
            env=build_config.env,
            run_from=run_from,
            project_working_subdir=project_working_subdir,
        )


if __name__ == '__main__':
    sys.exit(BazelBuildDriver().main())
