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
"""Preferences for pw_bluetooth."""

import os
import subprocess
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]
from pw_config_loader.yaml_config_loader_mixin import YamlConfigLoaderMixin

_DEFAULT_CONFIG: dict[str, Any] = {
    'fuchsia_checkout_path': None,
}

_DEFAULT_PROJECT_USER_FILE = (
    '$PW_PROJECT_ROOT/pw_bluetooth/.pw_bluetooth.user.yaml'
)


class BluetoothPrefs(YamlConfigLoaderMixin):
    """Bluetooth preferences storage class."""

    def __init__(
        self,
        project_file: Path | bool | None = None,
        project_user_file: (
            Path | str | bool | None
        ) = _DEFAULT_PROJECT_USER_FILE,
        user_file: Path | bool | None = None,
    ) -> None:
        if 'PW_PROJECT_ROOT' not in os.environ:
            for var in ('BUILD_WORKSPACE_DIRECTORY', 'PW_ROOT'):
                if var in os.environ:
                    os.environ['PW_PROJECT_ROOT'] = os.environ[var]
                    break
        else:
            try:
                root = subprocess.check_output(
                    ['git', 'rev-parse', '--show-toplevel'],
                    text=True,
                    stderr=subprocess.DEVNULL,
                ).strip()
                os.environ['PW_PROJECT_ROOT'] = root
            except (subprocess.SubprocessError, OSError):
                pass

        if isinstance(project_user_file, str):
            project_user_file = Path(os.path.expandvars(project_user_file))

        self.config_init(
            config_section_title='pw_bluetooth',
            project_file=project_file,
            project_user_file=project_user_file,
            user_file=user_file,
            default_config=_DEFAULT_CONFIG,
        )

    @property
    def fuchsia_checkout_path(self) -> Path | None:
        path = self._config.get('fuchsia_checkout_path')
        if path is None:
            return None
        return Path(os.path.expandvars(str(Path(path).expanduser())))

    def set_fuchsia_checkout_path(self, path: Path) -> None:
        """Sets the Fuchsia checkout path in the project user config file."""
        self._config['fuchsia_checkout_path'] = str(path)

        # Ensure we write to the project user file by default
        config_file = self.project_user_file
        if not config_file:
            # Fallback if somehow not set
            pw_root = os.environ.get('PW_PROJECT_ROOT') or os.environ.get(
                'PW_ROOT'
            )
            if not pw_root:
                raise RuntimeError(
                    "Could not determine project root to save config."
                )
            config_file = (
                Path(pw_root) / 'pw_bluetooth' / '.pw_bluetooth.user.yaml'
            )

        # Expand vars/user for writing
        write_path = Path(os.path.expandvars(str(config_file.expanduser())))

        # Load existing file if any to preserve other sections
        full_config: dict[str, Any] = {}
        if write_path.exists():
            with open(write_path, 'r') as f:
                loaded = yaml.safe_load(f)
                if isinstance(loaded, dict):
                    full_config = loaded

        # Update our section
        full_config['pw_bluetooth'] = self._config

        with open(write_path, 'w') as f:
            yaml.safe_dump(full_config, f)
