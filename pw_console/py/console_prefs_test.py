# Copyright 2021 The Pigweed Authors
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
"""Tests for pw_console.console_app"""

from pathlib import Path
import tempfile
import unittest

import yaml

# pylint: disable=protected-access
from pw_console.console_prefs import (
    ConsolePrefs,
    _DEFAULT_CONFIG,
)


def _create_tempfile(content: str) -> Path:
    with tempfile.NamedTemporaryFile(
        prefix=f'{__package__}', delete=False
    ) as output_file:
        output_file.write(content.encode('UTF-8'))
        return Path(output_file.name)


class TestConsolePrefs(unittest.TestCase):
    """Tests for ConsolePrefs."""

    maxDiff = None

    def test_load_no_existing_files(self) -> None:
        prefs = ConsolePrefs(
            project_file=False, project_user_file=False, user_file=False
        )
        self.assertEqual(_DEFAULT_CONFIG, prefs._config)
        self.assertTrue(str(prefs.repl_history).endswith('pw_console_history'))
        self.assertTrue(str(prefs.search_history).endswith('pw_console_search'))

    def test_load_empty_file(self) -> None:
        # Create an empty file
        project_config_file = _create_tempfile('')
        try:
            prefs = ConsolePrefs(
                project_file=project_config_file,
                project_user_file=False,
                user_file=False,
            )
            result_settings = {
                k: v
                for k, v in prefs._config.items()
                if k in _DEFAULT_CONFIG.keys()
            }
            other_settings = {
                k: v
                for k, v in prefs._config.items()
                if k not in _DEFAULT_CONFIG.keys()
            }
            # Check that only the default config was loaded.
            self.assertEqual(_DEFAULT_CONFIG, result_settings)
            self.assertEqual(0, len(other_settings))
        finally:
            project_config_file.unlink()

    def test_load_project_file(self) -> None:
        project_config = {
            'pw_console': {
                'ui_theme': 'light',
                'code_theme': 'cool-code',
                'swap_light_and_dark': True,
            },
        }
        project_config_file = _create_tempfile(yaml.dump(project_config))
        try:
            prefs = ConsolePrefs(
                project_file=project_config_file,
                project_user_file=False,
                user_file=False,
            )
            result_settings = {
                k: v
                for k, v in prefs._config.items()
                if k in project_config['pw_console'].keys()
            }
            other_settings = {
                k: v
                for k, v in prefs._config.items()
                if k not in project_config['pw_console'].keys()
            }
            self.assertEqual(project_config['pw_console'], result_settings)
            self.assertNotEqual(0, len(other_settings))
        finally:
            project_config_file.unlink()

    def test_load_project_and_user_file(self) -> None:
        """Test user settings override project settings."""
        project_config = {
            'pw_console': {
                'ui_theme': 'light',
                'code_theme': 'cool-code',
                'swap_light_and_dark': True,
                'repl_history': '~/project_history',
                'search_history': '~/project_search',
            },
        }
        project_config_file = _create_tempfile(yaml.dump(project_config))

        project_user_config = {
            'pw_console': {
                'ui_theme': 'nord',
                'repl_history': '~/project_user_history',
                'search_history': '~/project_user_search',
            },
        }
        project_user_config_file = _create_tempfile(
            yaml.dump(project_user_config)
        )

        user_config = {
            'pw_console': {
                'ui_theme': 'dark',
                'search_history': '~/user_search',
            },
        }
        user_config_file = _create_tempfile(yaml.dump(user_config))
        try:
            prefs = ConsolePrefs(
                project_file=project_config_file,
                project_user_file=project_user_config_file,
                user_file=user_config_file,
            )
            # Set by the project
            self.assertEqual(
                project_config['pw_console']['code_theme'], prefs.code_theme
            )
            self.assertEqual(
                project_config['pw_console']['swap_light_and_dark'],
                prefs.swap_light_and_dark,
            )

            # Project user setting, result should not be project only setting.
            project_history = project_config['pw_console']['repl_history']
            assert isinstance(project_history, str)
            self.assertNotEqual(
                Path(project_history).expanduser(), prefs.repl_history
            )

            history = project_user_config['pw_console']['repl_history']
            assert isinstance(history, str)
            self.assertEqual(Path(history).expanduser(), prefs.repl_history)

            # User config overrides project and project_user
            self.assertEqual(
                user_config['pw_console']['ui_theme'], prefs.ui_theme
            )
            self.assertEqual(
                Path(user_config['pw_console']['search_history']).expanduser(),
                prefs.search_history,
            )
            # ui_theme should not be the project_user file setting
            project_user_theme = project_user_config['pw_console']['ui_theme']
            self.assertNotEqual(project_user_theme, prefs.ui_theme)
        finally:
            project_config_file.unlink()
            project_user_config_file.unlink()
            user_config_file.unlink()

    def test_load_project_file_and_startup_window_options(self) -> None:
        """Test startup_window_options return values."""
        project_config = {
            'pw_console': {
                # Global table related startup_window_options:
                'column_order': [
                    'time',
                    'keys',
                    'level',
                ],
                'column_width': {
                    'timestamp': 9,
                    'module': 6,
                },
                'column_visibility': {
                    'timestamp': False,
                    'module': False,
                },
                'column_colors': {
                    'module': {
                        'default': '#fe4450',
                        'BAT': '#2ee2fa',
                        'RADIO': '#fede5d',
                        'CPU': '#ff7edb',
                        'APP': '#72f1b8 bold',
                        'USB': '#03edf9 bold',
                    }
                },
            },
        }
        project_config_file = _create_tempfile(yaml.dump(project_config))
        try:
            prefs = ConsolePrefs(
                project_file=project_config_file,
                project_user_file=False,
                user_file=False,
            )

            # Overridden window config values should be returned instead of
            # global values.
            window_config = {
                'wrap_lines': True,
                'table_mode': True,
                'column_order': [
                    'level',
                    'time',
                ],
                'column_width': {
                    'level': 6,
                    'timestamp': 12,
                    'module': 8,
                },
                'column_colors': {
                    'module': {
                        'default': '#ffffff',
                        'BAT': '#ff0000',
                    }
                },
                'column_visibility': {
                    'timestamp': True,
                    'module': True,
                },
            }
            self.assertEqual(
                prefs.column_order(window_config),
                [
                    'level',
                    'time',
                ],
            )
            self.assertEqual(
                prefs.column_width(window_config),
                {
                    'level': 6,
                    'timestamp': 12,
                    'module': 8,
                },
            )
            self.assertEqual(
                prefs.column_visibility(window_config),
                {
                    'timestamp': True,
                    'module': True,
                },
            )
            self.assertEqual(
                prefs.column_style(
                    'module', 'BAT', window_config=window_config
                ),
                '#ff0000',
            )
            self.assertEqual(
                prefs.column_style(
                    'module', 'unmatched value', window_config=window_config
                ),
                '#ffffff',
            )

            # Check global values are still returned normally.
            self.assertEqual(
                prefs.column_order(),
                [
                    'time',
                    'keys',
                    'level',
                ],
            )
            self.assertEqual(
                prefs.column_width(),
                {
                    'timestamp': 9,
                    'module': 6,
                },
            )
            self.assertEqual(
                prefs.column_visibility(),
                {
                    'timestamp': False,
                    'module': False,
                },
            )
            self.assertEqual(prefs.column_style('module', 'BAT'), '#2ee2fa')
            self.assertEqual(
                prefs.column_style('module', 'unmatched value'), '#fe4450'
            )

        finally:
            project_config_file.unlink()


if __name__ == '__main__':
    unittest.main()
