# Copyright 2020 The Pigweed Authors
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
"""The env module defines the environment variables used by Pigweed."""

from pathlib import Path
import os
import sys

from pw_cli import envparse
from pw_cli.allowed_caller import AllowedCaller, check_caller_in


def pigweed_environment_parser() -> envparse.EnvironmentParser:
    """Defines Pigweed's environment variables on an EnvironmentParser."""
    parser = envparse.EnvironmentParser(prefix='PW_')

    parser.add_var('PW_BOOTSTRAP_PYTHON')
    parser.add_var('PW_ENABLE_PRESUBMIT_HOOK_WARNING', default=False)
    parser.add_var('PW_EMOJI', type=envparse.strict_bool, default=False)
    parser.add_var('PW_ENVSETUP')
    parser.add_var('PW_ENVSETUP_FULL')
    parser.add_var(
        'PW_ENVSETUP_NO_BANNER', type=envparse.strict_bool, default=False
    )
    parser.add_var(
        'PW_ENVSETUP_QUIET', type=envparse.strict_bool, default=False
    )
    parser.add_var('PW_ENVIRONMENT_ROOT', type=Path)
    parser.add_var('PW_PACKAGE_ROOT', type=Path)
    parser.add_var('PW_PROJECT_ROOT', type=Path)
    parser.add_var('PW_ROOT', type=Path)
    parser.add_var(
        'PW_DISABLE_ROOT_GIT_REPO_CHECK',
        type=envparse.strict_bool,
        default=False,
    )
    parser.add_var('PW_SKIP_BOOTSTRAP')
    parser.add_var('PW_SUBPROCESS', type=envparse.strict_bool, default=False)
    parser.add_var('PW_USE_COLOR', type=envparse.strict_bool, default=True)
    parser.add_var('PW_USE_GCS_ENVSETUP', type=envparse.strict_bool)

    parser.add_allowed_suffix('_CIPD_INSTALL_DIR')

    parser.add_var(
        'PW_ENVSETUP_DISABLE_SPINNER', type=envparse.strict_bool, default=False
    )
    parser.add_var('PW_DOCTOR_SKIP_CIPD_CHECKS')
    parser.add_var(
        'PW_ACTIVATE_SKIP_CHECKS', type=envparse.strict_bool, default=False
    )

    parser.add_var('PW_BANNER_FUNC')
    parser.add_var('PW_BRANDING_BANNER')
    parser.add_var('PW_BRANDING_BANNER_COLOR', default='magenta')

    parser.add_var(
        'PW_PRESUBMIT_DISABLE_SUBPROCESS_CAPTURE', type=envparse.strict_bool
    )

    parser.add_var('PW_CONSOLE_CONFIG_FILE')
    parser.add_var('PW_ENVIRONMENT_NO_ERROR_ON_UNRECOGNIZED')

    parser.add_var('PW_NO_CIPD_CACHE_DIR')
    parser.add_var('PW_CIPD_SERVICE_ACCOUNT_JSON')

    # RBE environment variables
    parser.add_var('PW_USE_RBE', default=False)
    parser.add_var('PW_RBE_DEBUG', default=False)
    parser.add_var('PW_RBE_CLANG_CONFIG', default='')
    parser.add_var('PW_RBE_ARM_GCC_CONFIG', default='')

    parser.add_var(
        'PW_DISABLE_CLI_ANALYTICS', type=envparse.strict_bool, default=False
    )

    parser.add_var('PW_IDE_VERBOSE')

    return parser


# Internal: memoize environment parsing to avoid unnecessary computation in
# multiple calls to pigweed_environment().
_memoized_environment: envparse.EnvNamespace | None = None


def pigweed_environment() -> envparse.EnvNamespace:
    """Returns Pigweed's parsed environment."""
    global _memoized_environment  # pylint: disable=global-statement

    if _memoized_environment is None:
        _memoized_environment = pigweed_environment_parser().parse_env()

    return _memoized_environment


_BAZEL_PROJECT_ROOT_ALLOW_LIST = [
    AllowedCaller(
        filename='pw_build/py/pw_build/project_builder.py',
        name='*',
        function='__init__',
        self_class='ProjectBuilder',
    ),
    AllowedCaller(
        filename='pw_change/py/pw_change/review.py',
        name='__main__',
        function='review',
    ),
    AllowedCaller(
        filename='pw_watch/py/pw_watch/watch.py',
        name='__main__',
        function='watch_setup',
    ),
    AllowedCaller(
        filename='pw_watch/py/pw_watch/run.py',
        name='__main__',
        function='_parse_args',
    ),
]


_PROJECT_ROOT_ERROR_MESSAGE = '''
Error: Unable to determine the project root directory. Expected environment
variables are not set. Either $BUILD_WORKSPACE_DIRECTORY for bazel or
$PW_PROJECT_ROOT for Pigweed bootstrap are required.

Please re-run with either of these scenarios:

  1. Under bazel with "bazelisk run ..." or "bazel run ..."
  2. After activating a Pigweed bootstrap environment with ". ./activate.sh"
'''


def project_root(env: envparse.EnvNamespace | None = None) -> Path:
    """Returns the project root by checking bootstrap and bazel env vars.

    Please do not use this function unless the Python script must escape the
    bazel sandbox. For example, an interactive tool that operates on the project
    source code like code formatting.
    """

    if running_under_bazel():
        bazel_source_dir = os.environ.get('BUILD_WORKSPACE_DIRECTORY', '')

        # Ensure this function is only callable by functions in the allow list.
        check_caller_in(_BAZEL_PROJECT_ROOT_ALLOW_LIST)

        root = Path(bazel_source_dir)
    else:
        # Running outside bazel (via GN or bootstrap env).
        if env is None:
            env = pigweed_environment()
        root = env.PW_PROJECT_ROOT

    if not root:
        print(_PROJECT_ROOT_ERROR_MESSAGE, file=sys.stderr)
        sys.exit(1)

    return root


def running_under_bazel() -> bool:
    """Returns True if any bazel script environment variables are set.

    For more info on which variables are set when running executables in bazel
    see: https://bazel.build/docs/user-manual#running-executables
    """
    # The root of the workspace where the build was run.
    bazel_source_dir = os.environ.get('BUILD_WORKSPACE_DIRECTORY', '')
    # The current working directory where Bazel was run from.
    bazel_working_dir = os.environ.get('BUILD_WORKING_DIRECTORY', '')

    return bool(bazel_source_dir or bazel_working_dir)
