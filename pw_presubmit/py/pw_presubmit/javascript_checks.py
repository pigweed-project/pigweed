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
"""JavaScript and TypeScript validity check."""

from pw_presubmit import presubmit_context
from pw_presubmit.presubmit import (
    Check,
    filter_paths,
)
from pw_presubmit.presubmit_context import PresubmitContext
from pw_presubmit.private.result import PresubmitFailure
from pw_presubmit.tools import log_run


@filter_paths(endswith=('.js', '.ts'))
@Check
def eslint(ctx: PresubmitContext):
    """Presubmit check that ensures JavaScript files are valid."""

    ctx.paths = presubmit_context.apply_exclusions(ctx)

    # Check if npm deps are installed.
    npm_list = log_run(['npm', 'list'], cwd='pw_web')
    if npm_list.returncode != 0:
        npm_install = log_run(
            ['npm', 'install', '--ignore-scripts'], cwd='pw_web'
        )
        if npm_install.returncode != 0:
            raise PresubmitFailure('npm install failed.')

    result = log_run(
        [
            'npm',
            'exec',
            'eslint',
            '--',
            '--config',
            '.eslintrc.cjs',
            *ctx.paths,
        ],
        cwd='pw_web',
    )
    if result.returncode != 0:
        raise PresubmitFailure('eslint identifed issues.')
