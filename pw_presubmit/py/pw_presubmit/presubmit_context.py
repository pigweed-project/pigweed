# Copyright 2023 The Pigweed Authors
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
"""pw_presubmit ContextVar."""

from __future__ import annotations

from contextvars import ContextVar
import dataclasses

import inspect
import logging
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
from typing import (
    Any,
    Iterable,
    Sequence,
)
import urllib.parse

import pw_cli.color
import pw_cli.env
import pw_env_setup.config_file

from pw_presubmit.private.check import PresubmitCheckTrace
from pw_presubmit.private.result import Failure

_COLOR = pw_cli.color.colors()
_LOG: logging.Logger = logging.getLogger(__name__)

PRESUBMIT_CHECK_TRACE: ContextVar[dict[str, list[PresubmitCheckTrace]]] = (
    ContextVar('pw_presubmit_check_trace', default={})
)


# TODO: b/433258471 - Pass in project root rather than getting it from the env.
def _project_root() -> Path:
    # Check if running from Bazel
    if 'BUILD_WORKSPACE_DIRECTORY' in os.environ:
        return Path(os.environ['BUILD_WORKSPACE_DIRECTORY']).resolve()

    if 'TEST_SRCDIR' in os.environ:
        return Path(os.environ['TEST_SRCDIR']).resolve()

    # Fall back to bootstrapped environment
    return Path(pw_cli.env.pigweed_environment().PW_PROJECT_ROOT)


@dataclasses.dataclass(frozen=True)
class FormatOptions:
    """Formatting options, derived from pigweed.json."""

    python_formatter: str | None = 'black'
    black_path: str | None = 'black'
    exclude: Sequence[re.Pattern] = dataclasses.field(default_factory=list)

    @staticmethod
    def load(env: dict[str, str] | None = None) -> FormatOptions:
        if (
            'BUILD_WORKING_DIRECTORY' in os.environ
            or 'TEST_SRCDIR' in os.environ
        ):
            _LOG.debug('Running from Bazel; using default FormatOptions')
            return FormatOptions()

        config = pw_env_setup.config_file.load(env=env)
        fmt = config.get('pw', {}).get('pw_presubmit', {}).get('format', {})
        return FormatOptions(
            python_formatter=fmt.get('python_formatter', 'black'),
            black_path=fmt.get('black_path', 'black'),
            exclude=tuple(re.compile(x) for x in fmt.get('exclude', ())),
        )

    def filter_paths(self, paths: Iterable[Path]) -> tuple[Path, ...]:
        root = _project_root()
        relpaths = [x.relative_to(root) for x in paths]

        for filt in self.exclude:
            relpaths = [x for x in relpaths if not filt.search(str(x))]
        return tuple(root / x for x in relpaths)


def get_buildbucket_info(bbid) -> dict[str, Any]:
    if not bbid or not shutil.which('bb'):
        return {}

    output = subprocess.check_output(
        ['bb', 'get', '-json', '-p', f'{bbid}'], text=True
    )
    return json.loads(output)


@dataclasses.dataclass
class LuciPipeline:
    """Details of previous builds in this pipeline, if applicable.

    Attributes:
        round: The zero-indexed round number.
        builds_from_previous_iteration: A list of the buildbucket ids from the
            previous round, if any.
    """

    round: int
    builds_from_previous_iteration: Sequence[int]

    @staticmethod
    def create(
        bbid: int,
        fake_pipeline_props: dict[str, Any] | None = None,
    ) -> LuciPipeline | None:
        pipeline_props: dict[str, Any]
        if fake_pipeline_props is not None:
            pipeline_props = fake_pipeline_props
        else:
            pipeline_props = (
                get_buildbucket_info(bbid)
                .get('input', {})
                .get('properties', {})
                .get('$pigweed/pipeline', {})
            )
        if not pipeline_props.get('inside_a_pipeline', False):
            return None

        return LuciPipeline(
            round=int(pipeline_props['round']),
            builds_from_previous_iteration=list(
                int(x) for x in pipeline_props['builds_from_previous_iteration']
            ),
        )


@dataclasses.dataclass
class LuciTrigger:
    """Details the pending change or submitted commit triggering the build.

    Attributes:
        number: The number of the change in Gerrit.
        patchset: The number of the patchset of the change.
        remote: The full URL of the remote.
        project: The name of the project in Gerrit.
        branch: The name of the branch on which this change is being/was
            submitted.
        ref: The "refs/changes/.." path that can be used to reference the
            patch for unsubmitted changes and the hash for submitted changes.
        gerrit_name: The name of the googlesource.com Gerrit host.
        submitted: Whether the change has been submitted or is still pending.
        primary: Whether this change was the change that triggered a build or
            if it was imported by that triggering change.
        gerrit_host: The scheme and hostname of the googlesource.com Gerrit
            host.
        gerrit_url: The full URL to this change on the Gerrit host.
        gitiles_url: The full URL to this commit in Gitiles.
    """

    number: int
    patchset: int
    remote: str
    project: str
    branch: str
    ref: str
    gerrit_name: str
    submitted: bool
    primary: bool

    @property
    def gerrit_host(self):
        return f'https://{self.gerrit_name}-review.googlesource.com'

    @property
    def gerrit_url(self):
        if not self.number:
            return self.gitiles_url
        return f'{self.gerrit_host}/c/{self.number}'

    @property
    def gitiles_url(self):
        return f'{self.remote}/+/{self.ref}'

    @staticmethod
    def create_from_environment(
        env: dict[str, str] | None = None,
    ) -> Sequence['LuciTrigger']:
        """Create a LuciTrigger from the environment."""
        if not env:
            env = os.environ.copy()
        raw_path = env.get('TRIGGERING_CHANGES_JSON')
        if not raw_path:
            return ()
        path = Path(raw_path)
        if not path.is_file():
            return ()

        result = []
        with open(path, 'r') as ins:
            for trigger in json.load(ins):
                keys = {
                    'number',
                    'patchset',
                    'remote',
                    'project',
                    'branch',
                    'ref',
                    'gerrit_name',
                    'submitted',
                    'primary',
                }
                if keys <= trigger.keys():
                    result.append(LuciTrigger(**{x: trigger[x] for x in keys}))

        return tuple(result)

    @staticmethod
    def create_for_testing(**kwargs):
        """Create a LuciTrigger for testing."""
        change = {
            'number': 123456,
            'patchset': 1,
            'remote': 'https://pigweed.googlesource.com/pigweed/pigweed',
            'project': 'pigweed/pigweed',
            'branch': 'main',
            'ref': 'refs/changes/56/123456/1',
            'gerrit_name': 'pigweed',
            'submitted': True,
            'primary': True,
        }
        change.update(kwargs)

        with tempfile.TemporaryDirectory() as tempdir:
            changes_json = Path(tempdir) / 'changes.json'
            with changes_json.open('w') as outs:
                json.dump([change], outs)
            env = {'TRIGGERING_CHANGES_JSON': changes_json}
            return LuciTrigger.create_from_environment(env)


@dataclasses.dataclass
class LuciContext:  # pylint: disable=too-many-instance-attributes
    """LUCI-specific information about the environment.

    Attributes:
        buildbucket_id: The globally-unique buildbucket id of the build.
        build_number: The builder-specific incrementing build number, if
            configured for this builder.
        project: The LUCI project under which this build is running (often
            "pigweed" or "pigweed-internal").
        bucket: The LUCI bucket under which this build is running (often ends
            with "ci" or "try").
        builder: The builder being run.
        tags: The buildbucket tags applied to this build.
        swarming_server: The swarming server on which this build is running.
        swarming_task_id: The swarming task id of this build.
        cas_instance: The CAS instance accessible from this build.
        context_file: The path to the LUCI_CONTEXT file.
        pipeline: Information about the build pipeline, if applicable.
        triggers: Information about triggering commits, if applicable.
        is_try: True if the bucket is a try bucket.
        is_ci: True if the bucket is a ci bucket.
        is_dev: True if the bucket is a dev bucket.
        is_shadow: True if the bucket is a shadow bucket.
        is_prod: True if both is_dev and is_shadow are False.
    """

    buildbucket_id: int
    build_number: int
    project: str
    bucket: str
    builder: str
    tags: Sequence[tuple[str, str]]
    swarming_server: str
    swarming_task_id: str
    cas_instance: str
    context_file: Path
    pipeline: LuciPipeline | None
    triggers: Sequence[LuciTrigger] = dataclasses.field(default_factory=tuple)

    @property
    def is_try(self):
        return 'try' in self.bucket.split('.')

    @property
    def is_ci(self):
        return 'ci' in self.bucket.split('.')

    @property
    def is_dev(self):
        return 'dev' in self.bucket.split('.')

    @property
    def is_shadow(self):
        return 'shadow' in self.bucket.split('.')

    @property
    def is_prod(self):
        return not self.is_dev and not self.is_shadow

    @staticmethod
    def create_from_environment(
        env: dict[str, str] | None = None,
        fake_pipeline_props: dict[str, Any] | None = None,
    ) -> LuciContext | None:
        """Create a LuciContext from the environment."""

        if not env:
            env = os.environ.copy()

        luci_vars = [
            'BUILDBUCKET_METADATA_JSON',
            'LUCI_CONTEXT',
            'SWARMING_TASK_ID',
            'SWARMING_SERVER',
        ]
        if any(x for x in luci_vars if x not in env):
            return None

        with Path(env['BUILDBUCKET_METADATA_JSON']).open() as ins:
            bb_metadata = json.load(ins)

        project = bb_metadata['project']
        bucket = bb_metadata['bucket']
        builder = bb_metadata['builder']

        bbid = int(bb_metadata['id'])
        number = int(bb_metadata['number'])
        pipeline = LuciPipeline.create(bbid, fake_pipeline_props)

        tags = tuple(bb_metadata['tags'])

        # Logic to identify cas instance from swarming server is derived from
        # https://chromium.googlesource.com/infra/luci/recipes-py/+/main/recipe_modules/cas/api.py
        swarm_server = env['SWARMING_SERVER']
        cas_project = urllib.parse.urlparse(swarm_server).netloc.split('.')[0]
        cas_instance = f'projects/{cas_project}/instances/default_instance'

        result = LuciContext(
            buildbucket_id=bbid,
            build_number=int(number),
            project=project,
            bucket=bucket,
            builder=builder,
            tags=tags,
            swarming_server=env['SWARMING_SERVER'],
            swarming_task_id=env['SWARMING_TASK_ID'],
            cas_instance=cas_instance,
            pipeline=pipeline,
            triggers=LuciTrigger.create_from_environment(env),
            context_file=Path(env['LUCI_CONTEXT']),
        )
        _LOG.debug('%r', result)
        return result

    @staticmethod
    def create_for_testing(**kwargs):
        """Easily create a LuciContext for testing."""
        with tempfile.TemporaryDirectory() as tempdir:
            name = kwargs.pop(
                'BUILDBUCKET_NAME',
                'pigweed:bucket.try:builder-name',
            )
            project, bucket, builder = name.split(':')

            bb_metadata = {
                'id': kwargs.pop('BUILDBUCKET_ID', '881234567890'),
                'number': kwargs.pop('BUILD_NUMBER', '123'),
                'project': project,
                'bucket': bucket,
                'builder': builder,
                'tags': kwargs.pop('tags', [('key', 'value')]),
            }

            swarming_server = kwargs.pop(
                'SWARMING_SERVER',
                'https://chromium-swarm.appspot.com',
            )
            swarming_task_id = kwargs.pop('SWARMING_TASK_ID', 'cd2dac62d2')

            if kwargs:
                raise ValueError(f'unexpected kwargs: {kwargs}')

            json_path = Path(tempdir) / 'bbmetadata.json'

            with json_path.open('w') as outs:
                json.dump(bb_metadata, outs)

            env = {
                'BUILDBUCKET_METADATA_JSON': str(json_path),
                'LUCI_CONTEXT': '/path/to/context/file.json',
                'SWARMING_SERVER': swarming_server,
                'SWARMING_TASK_ID': swarming_task_id,
            }
            env.update(kwargs)

            return LuciContext.create_from_environment(env, {})


@dataclasses.dataclass
class FormatContext:
    """Context passed into formatting helpers.

    This class is a subset of PresubmitContext containing only what's needed by
    formatters.

    For full documentation on the members see the PresubmitContext section of
    pw_presubmit/docs.rst.

    Attributes:
        root: Source checkout root directory
        paths: Modified files for the presubmit step to check (often used in
            formatting steps but ignored in compile steps).
        format_options: Formatting options, derived from pigweed.json.
        dry_run: Whether to just report issues or also fix them.
    """

    root: Path | None
    paths: tuple[Path, ...]
    format_options: FormatOptions
    dry_run: bool = False

    def append_check_command(self, *command_args, **command_kwargs) -> None:
        """Empty append_check_command."""


@dataclasses.dataclass
class PresubmitContext:  # pylint: disable=too-many-instance-attributes
    """Context passed into presubmit checks.

    For full documentation on the members see pw_presubmit/docs.rst.

    Attributes:
        root: Source checkout root directory.
        repos: Repositories (top-level and submodules) processed by
            `pw presubmit`.
        output_dir: Output directory for this specific presubmit step.

        paths: Modified files for the presubmit step to check (often used in
            formatting steps but ignored in compile steps).
        all_paths: All files in the tree.
        package_root: Root directory for pw package installations.
        override_gn_args: Additional GN args processed by `build.gn_gen()`.
        luci: Information about the LUCI build or None if not running in LUCI.
        format_options: Formatting options, derived from pigweed.json.
        num_jobs: Number of jobs to run in parallel.
        continue_after_build_error: For steps that compile, don't exit on the
            first compilation error.
        rng_seed: Seed for a random number generator, for the few steps that
            need one.
        full: Whether this is a full or incremental presubmit run.
        _failures: List presubmit failures; recorded by calling ctx.fail().
        dry_run: Whether to actually execute commands or just log them.
        use_remote_cache: Whether to tell the build system to use RBE.
    """

    root: Path
    repos: tuple[Path, ...]
    output_dir: Path
    paths: tuple[Path, ...]
    all_paths: tuple[Path, ...]
    package_root: Path
    luci: LuciContext | None
    override_gn_args: dict[str, str]
    format_options: FormatOptions
    num_jobs: int | None = None
    continue_after_build_error: bool = False
    rng_seed: int = 1
    full: bool = False
    _failures: list[Failure] = dataclasses.field(default_factory=list)
    dry_run: bool = False
    use_remote_cache: bool = False

    @property
    def failed(self) -> bool:
        return bool(self._failures)

    @property
    def incremental(self) -> bool:
        return not self.full

    def fail(
        self,
        description: str,
        path: Path | None = None,
        line: int | None = None,
    ):
        """Add a failure to this presubmit step.

        If this is called at least once the step fails, but not immediately—the
        check is free to continue and possibly call this method again.
        """
        failure = Failure(description, path, line)
        _LOG.warning('%s', failure)
        self._failures.append(failure)

    @staticmethod
    def create_for_testing(**kwargs):
        root = Path.cwd()
        presubmit_root = root / 'out' / 'presubmit'
        presubmit_kwargs = {
            'root': root,
            'repos': (root,),
            'output_dir': presubmit_root / 'test',
            'paths': (root / 'foo.cc', root / 'foo.py'),
            'all_paths': (root / 'BUILD.gn', root / 'foo.cc', root / 'foo.py'),
            'package_root': root / 'environment' / 'packages',
            'luci': None,
            'override_gn_args': {},
            'format_options': FormatOptions(),
        }
        presubmit_kwargs.update(kwargs)
        return PresubmitContext(**presubmit_kwargs)

    def append_check_command(
        self,
        *command_args,
        call_annotation: dict[Any, Any] | None = None,
        **command_kwargs,
    ) -> None:
        """Save a subprocess command annotation to this presubmit context.

        This is used to capture commands that will be run for display in ``pw
        presubmit --dry-run.``

        Args:

            command_args: All args that would normally be passed to
                subprocess.run

            call_annotation: Optional key value pairs of data to save for this
                command. Examples:

                ::

                   call_annotation={'pw_package_install': 'teensy'}
                   call_annotation={'build_system': 'bazel'}
                   call_annotation={'build_system': 'ninja'}

            command_kwargs: keyword args that would normally be passed to
                subprocess.run.
        """
        call_annotation = call_annotation if call_annotation else {}
        calling_func: str | None = None
        calling_check = None

        # Loop through the current call stack looking for `self`, and stopping
        # when self is a Check() instance and if the __call__ or _try_call
        # functions are in the stack.

        # This used to be an isinstance(obj, Check) call, but it was changed to
        # this so Check wouldn't need to be imported here. Doing so would create
        # a dependency loop.
        def is_check_object(obj):
            return getattr(obj, '_is_presubmit_check_object', False)

        for frame_info in inspect.getouterframes(inspect.currentframe()):
            self_obj = frame_info.frame.f_locals.get('self', None)
            if (
                self_obj
                and is_check_object(self_obj)
                and frame_info.function in ['_try_call', '__call__']
            ):
                calling_func = frame_info.function
                calling_check = self_obj

        save_check_trace(
            self.output_dir,
            PresubmitCheckTrace(
                self,
                calling_check,
                calling_func,
                command_args,
                command_kwargs,
                call_annotation,
            ),
        )

    def __post_init__(self) -> None:
        PRESUBMIT_CONTEXT.set(self)

    def __hash__(self):
        return hash(
            tuple(
                (
                    tuple(attribute.items())
                    if isinstance(attribute, dict)
                    else attribute
                )
                for attribute in dataclasses.astuple(self)
            )
        )


PRESUBMIT_CONTEXT: ContextVar[PresubmitContext | None] = ContextVar(
    'pw_presubmit_context', default=None
)


def get_presubmit_context():
    return PRESUBMIT_CONTEXT.get()


def save_check_trace(output_dir: Path, trace: PresubmitCheckTrace) -> None:
    trace_key = str(output_dir.resolve())
    trace_list = PRESUBMIT_CHECK_TRACE.get().get(trace_key, [])
    trace_list.append(trace)
    PRESUBMIT_CHECK_TRACE.get()[trace_key] = trace_list


def get_check_traces(ctx: PresubmitContext) -> list[PresubmitCheckTrace]:
    trace_key = str(ctx.output_dir.resolve())
    return PRESUBMIT_CHECK_TRACE.get().get(trace_key, [])


def log_check_traces(ctx: PresubmitContext) -> None:
    traces = PRESUBMIT_CHECK_TRACE.get()

    for _output_dir, check_traces in traces.items():
        for check_trace in check_traces:
            if check_trace.ctx != ctx:
                continue

            quoted_command_args = ' '.join(
                shlex.quote(str(arg)) for arg in check_trace.args
            )
            _LOG.info(
                '%s %s',
                _COLOR.blue('Run ==>'),
                quoted_command_args,
            )


def apply_exclusions(
    ctx: PresubmitContext,
    paths: Sequence[Path] | None = None,
) -> tuple[Path, ...]:
    return ctx.format_options.filter_paths(paths or ctx.paths)
