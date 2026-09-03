# Copyright 2022 The Pigweed Authors
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
"""Presubmit to npm install and run tests"""

import os
import shutil
from pw_presubmit.presubmit import (
    Check,
    call,
    filter_paths,
)
from pw_presubmit.presubmit_context import PresubmitContext


@filter_paths(
    endswith=(
        '.js',
        '.ts',
        '.tsx',
        'package.json',
        'tsconfig.json',
        'pnpm-lock.yaml',
    )
)
@Check
def npm_test(ctx: PresubmitContext) -> None:
    """Run npm install and npm test in Pigweed root to test all web modules"""
    web_dir = ctx.root / 'pw_web'
    call('npm', "install", cwd=web_dir)
    call('npm', "test", cwd=web_dir)


def vscode_test(ctx: PresubmitContext) -> None:
    """Run npm install and npm run test:all to test the VS Code extension."""
    vsc_dir = ctx.root / 'pw_ide' / 'ts' / 'pigweed_vscode'
    npm = shutil.which('npm.cmd' if os.name == 'nt' else 'npm')
    call(npm, 'install', cwd=vsc_dir)
    call(npm, 'run', 'test:all', cwd=vsc_dir)
