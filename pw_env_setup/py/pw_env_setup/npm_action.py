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
"""NPM pw_env_setup action plugin.

This action triggers `npm install` after CIPD setup.
"""
import os
from pathlib import Path
import shutil
import stat
from string import Template
import subprocess


# The list of NPM tools that should be added to the PATH after installation
# finishes. We do this by an allowlist since NPM has tools like `protoc` that we
# never want to get added to the PATH.
NPM_TOOLS = ('prettier',)


# We need to use wrapper scripts rather than symlinks because the .cmd NPM
# scripts on Windows are wrappers that do path-relative execution of NPM
# modules. If we symlink to those, the relative path resolution breaks.
_WINDOWS_NPM_LAUNCHER_TEMPLATE = """@echo off
call ${program} %*
"""

_UNIX_NPM_LAUNCHER_TEMPLATE = """#!/usr/bin/env bash
${program} $$@
"""


def run_action(env=None):
    """Project action to run 'npm install'."""
    if not env:
        raise ValueError(f"Missing 'env', got %{env}")

    with env():
        npm = shutil.which('npm.cmd' if os.name == 'nt' else 'npm')

        repo_root = os.environ.get('PW_ROOT') or os.environ.get(
            'PW_PROJECT_ROOT'
        )

        # TODO: b/323378974 - Better integrate NPM actions with pw_env_setup so
        # we don't have to manually set `npm_config_cache` every time we run
        # npm.
        # Force npm cache to live inside the environment directory.
        npm_env = os.environ.copy()
        npm_env['npm_config_cache'] = os.path.join(
            npm_env['_PW_ACTUAL_ENVIRONMENT_ROOT'], 'npm-cache'
        )

        web_dir = Path(repo_root) / 'pw_web'

        subprocess.run(
            [
                npm,
                'install',
                '--ignore-scripts',
                '--quiet',
                '--no-progress',
                '--loglevel=error',
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            cwd=web_dir,
            env=npm_env,
            check=True,
        )

        # Add symlinks to desired NPM tools to $PW_WEB_CIPD_INSTALL_DIR/bin.
        bin_dir = Path(os.environ.get('PW_WEB_CIPD_INSTALL_DIR')) / 'bin'
        if not bin_dir.exists():
            bin_dir.mkdir()

        for tool in NPM_TOOLS:
            proc = subprocess.run(
                [
                    npm,
                    'exec',
                    'which',
                    tool,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=web_dir,
                env=npm_env,
                check=True,
            )

            tool_path = Path(proc.stdout.decode().strip())
            extension = '.bat' if os.name == 'nt' else ''
            dest = bin_dir / f'{tool}{extension}'
            template = Template(
                _WINDOWS_NPM_LAUNCHER_TEMPLATE
                if os.name == 'nt'
                else _UNIX_NPM_LAUNCHER_TEMPLATE
            )
            dest.write_text(template.substitute(program=tool_path))
            dest.chmod(dest.stat().st_mode | stat.S_IEXEC)
