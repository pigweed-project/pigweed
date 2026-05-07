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
"""pw_console preferences"""

from __future__ import annotations

import copy
import dataclasses
import os
from pathlib import Path
import sys
from typing import Any, Callable

from prompt_toolkit.key_binding import KeyBindings
import yaml

from pw_config_loader.yaml_config_loader_mixin import YamlConfigLoaderMixin

from pw_console.style import generate_styles
from pw_console.key_bindings import DEFAULT_KEY_BINDINGS

_DEFAULT_REPL_HISTORY: Path = Path.home() / '.pw_console_history'
_DEFAULT_SEARCH_HISTORY: Path = Path.home() / '.pw_console_search'

_DEFAULT_CONFIG = {
    # History files
    'repl_history': _DEFAULT_REPL_HISTORY,
    'search_history': _DEFAULT_SEARCH_HISTORY,
    # Appearance
    'ui_theme': 'dark',
    'code_theme': 'pigweed-code',
    'swap_light_and_dark': False,
    'spaces_between_columns': 2,
    'column_order_omit_unspecified_columns': False,
    'column_order': [],
    'column_visibility': {},
    'column_width': {},
    'column_colors': {},
    'show_python_file': False,
    'show_python_logger': False,
    'show_source_file': False,
    'hide_date_from_log_time': False,
    'recolor_log_lines_to_match_level': True,
    # Window arrangement
    'windows': {},
    'window_column_split_method': 'vertical',
    'command_runner': {
        'width': 80,
        'height': 10,
        'position': {'top': 3},
    },
    'key_bindings': DEFAULT_KEY_BINDINGS,
    'snippets': {},
    'user_snippets': {},
}

_DEFAULT_PROJECT_FILE = Path('$PW_PROJECT_ROOT/.pw_console.yaml')
_DEFAULT_PROJECT_USER_FILE = Path('$PW_PROJECT_ROOT/.pw_console.user.yaml')
_DEFAULT_USER_FILE = Path('$HOME/.pw_console.yaml')


class EmptyPreviousPreviousDescription(Exception):
    """Previous snippet description is empty for 'description: USE_PREVIOUS'."""


@dataclasses.dataclass
class CodeSnippet:
    """Stores a single code snippet for inserting into the Python Repl.

    Attributes:

        title: The displayed title in the command runner window.
        code: Python code text to be inserted.
        description: Optional help text to be displayed below the snippet
            selection window.
    """

    title: str
    code: str
    description: str | None = None

    @staticmethod
    def from_yaml(
        title: str,
        value: str | dict,
        previous_description: str | None = None,
    ) -> CodeSnippet:
        if isinstance(value, str):
            return CodeSnippet(title=title, code=value)

        assert isinstance(value, dict)

        code = value.get('code', '')
        description = value.get('description', None)
        if description == 'USE_PREVIOUS':
            if not previous_description:
                raise EmptyPreviousPreviousDescription(
                    f'\nERROR: pw_console.yaml snippet "{title}" has '
                    '"description: USE_PREVIOUS" but the previous snippet '
                    'description is empty.'
                )
            description = previous_description
        return CodeSnippet(title=title, code=code, description=description)


def warn_unknown_window(
    window_title: str, existing_pane_titles: list[str]
) -> None:
    """Print a warning when a window config has an unknown title.

    If a window title does not already exist on startup it must have a loggers:
    command: or duplicate_of: option set."""

    pane_title_text = '  ' + '\n  '.join(existing_pane_titles)
    existing_pane_title_example = 'Window Title'
    if existing_pane_titles:
        existing_pane_title_example = existing_pane_titles[0]

    print(
        f'WARNING: The window "{window_title}" '
        'specified in a pw_console.yaml file does not exist.\n'
        'Existing windows include:\n'
        f'{pane_title_text}\n'
        'If this window should be a duplicate of one of the above:\n'
        f'  Add "duplicate_of: {existing_pane_title_example}" to your config.\n'
        'If this is a brand new window:\n'
        '  Add a "loggers:" or "command:" section to the window spec.\n'
        'For examples see: \n'
        'https://pigweed.dev/pw_console/py/pw_console/docs/user_guide.html'
        '#example-config\n',
        end=None,
        file=sys.stderr,
    )


def warn_empty_window_list(
    window_list_title: str,
) -> None:
    """Print a warning if a window list is empty."""
    print(
        f'WARNING: The window layout heading "{window_list_title}" '
        'specified in a pw_console.yaml file '
        'contains no windows.\n'
        'For examples see: \n'
        'https://pigweed.dev/pw_console/py/pw_console/docs/user_guide.html'
        '#example-config\n',
        end=None,
        file=sys.stderr,
    )


class ConsolePrefs(YamlConfigLoaderMixin):
    """Pigweed Console preferences storage class."""

    # pylint: disable=too-many-public-methods

    def __init__(
        self,
        project_file: Path | bool = _DEFAULT_PROJECT_FILE,
        project_user_file: Path | bool = _DEFAULT_PROJECT_USER_FILE,
        user_file: Path | bool = _DEFAULT_USER_FILE,
    ) -> None:
        self.config_init(
            config_section_title='pw_console',
            project_file=project_file,
            project_user_file=project_user_file,
            user_file=user_file,
            default_config=_DEFAULT_CONFIG,
            environment_var='PW_CONSOLE_CONFIG_FILE',
        )

        self._snippet_completions: list[CodeSnippet] = []
        self.registered_commands = DEFAULT_KEY_BINDINGS
        self.registered_commands.update(self.user_key_bindings)

    @property
    def ui_theme(self) -> str:
        return self._config.get('ui_theme', '')

    @ui_theme.setter
    def ui_theme(self, theme_name: str) -> None:
        self._config['ui_theme'] = theme_name

    @property
    def ui_themes(self) -> dict:
        return self._config.get('ui_themes', {})

    @property
    def code_theme(self) -> str:
        return self._config.get('code_theme', '')

    @code_theme.setter
    def code_theme(self, theme_name: str) -> None:
        self._config['code_theme'] = theme_name

    @property
    def swap_light_and_dark(self) -> bool:
        return self._config.get('swap_light_and_dark', False)

    @property
    def repl_history(self) -> Path:
        history = Path(self._config['repl_history'])
        history = Path(os.path.expandvars(str(history.expanduser())))
        return history

    @property
    def search_history(self) -> Path:
        history = Path(self._config['search_history'])
        history = Path(os.path.expandvars(str(history.expanduser())))
        return history

    @property
    def spaces_between_columns(self) -> int:
        spaces = self._config.get('spaces_between_columns', 2)
        assert isinstance(spaces, int) and spaces > 0
        return spaces

    @property
    def omit_unspecified_columns(self) -> bool:
        return self._config.get('column_order_omit_unspecified_columns', False)

    @property
    def hide_date_from_log_time(self) -> bool:
        return self._config.get('hide_date_from_log_time', False)

    @property
    def recolor_log_lines_to_match_level(self) -> bool:
        return self._config.get('recolor_log_lines_to_match_level', True)

    @property
    def show_python_file(self) -> bool:
        return self._config.get('show_python_file', False)

    @property
    def show_source_file(self) -> bool:
        return self._config.get('show_source_file', False)

    @property
    def show_python_logger(self) -> bool:
        return self._config.get('show_python_logger', False)

    def toggle_bool_option(self, name: str):
        existing_setting = self._config[name]
        assert isinstance(existing_setting, bool)
        self._config[name] = not existing_setting

    def column_order(
        self, window_config: dict[str, Any] | None = None
    ) -> list[str]:
        if window_config is None:
            window_config = {}
        global_config = self._config.get('column_order', [])
        window_override = window_config.get('column_order', [])
        if window_override:
            return window_override
        return global_config

    def column_width(
        self, window_config: dict[str, Any] | None = None
    ) -> dict[str, int]:
        if window_config is None:
            window_config = {}
        final_config = copy.deepcopy(self._config.get('column_width', {}))
        final_config.update(window_config.get('column_width', {}))
        return {
            name: int(width)
            for name, width in final_config.items()
            if name.lower() != 'message'
        }

    def column_visibility(
        self, window_config: dict[str, Any] | None = None
    ) -> dict[str, bool]:
        if window_config is None:
            window_config = {}
        final_config = copy.deepcopy(self._config.get('column_visibility', {}))
        final_config.update(window_config.get('column_visibility', {}))
        return {
            name: is_visible
            for name, is_visible in final_config.items()
            if name.lower() != 'message'
        }

    def column_style(
        self,
        column_name: str,
        column_value: str,
        default='',
        window_config: dict[str, Any] | None = None,
    ) -> str:
        if window_config is None:
            window_config = {}
        column_colors = copy.deepcopy(self._config.get('column_colors', {}))
        column_colors.update(window_config.get('column_colors', {}))
        column_style = default

        if column_name in column_colors:
            # If key exists but doesn't have any values.
            if not column_colors[column_name]:
                return default
            # Check for user supplied default.
            column_style = column_colors[column_name].get('default', default)
            # Check for value specific color, otherwise use the default.
            column_style = column_colors[column_name].get(
                column_value, column_style
            )
        return column_style

    def pw_console_color_config(self) -> dict[str, dict]:
        column_colors = self._config.get('column_colors', {})
        theme_styles = generate_styles(self.ui_theme)
        style_classes = dict(theme_styles.style_rules)

        color_config = {}
        color_config['classes'] = style_classes
        color_config['column_values'] = column_colors
        return {'__pw_console_colors': color_config}

    @property
    def window_column_split_method(self) -> str:
        default_value = 'vertical'
        legacy_value = self._config.get(
            'window_column_split_method', default_value
        )
        value = self._config.get('window_group_split_method', legacy_value)
        return value

    @property
    def windows(self) -> dict:
        return self._config.get('windows', {})

    def set_windows(self, new_config: dict) -> None:
        self._config['windows'] = new_config

    @property
    def window_column_modes(self) -> list:
        return list(column_type for column_type in self.windows.keys())

    @property
    def command_runner_position(self) -> dict[str, int]:
        position = self._config.get('command_runner', {}).get(
            'position', {'top': 3}
        )
        return {
            key: value
            for key, value in position.items()
            if key in ['top', 'bottom', 'left', 'right']
        }

    @property
    def command_runner_width(self) -> int:
        return self._config.get('command_runner', {}).get('width', 80)

    @property
    def command_runner_height(self) -> int:
        return self._config.get('command_runner', {}).get('height', 10)

    @property
    def user_key_bindings(self) -> dict[str, list[str]]:
        return self._config.get('key_bindings', {})

    def current_config_as_yaml(self) -> str:
        yaml_options = dict(
            sort_keys=True, default_style='', default_flow_style=False
        )

        title = {'config_title': 'pw_console'}
        text = '\n'
        text += yaml.safe_dump(title, **yaml_options)  # type: ignore

        keys = {'key_bindings': self.registered_commands}
        text += '\n'
        text += yaml.safe_dump(keys, **yaml_options)  # type: ignore

        return text

    @property
    def unique_window_titles(self) -> set:
        titles = []
        for window_list_title, column in self.windows.items():
            if not column:
                warn_empty_window_list(window_list_title)
                continue

            for window_key_title, window_dict in column.items():
                window_options = window_dict if window_dict else {}
                # Use 'duplicate_of: Title' if it exists, otherwise use the key.
                titles.append(
                    window_options.get('duplicate_of', window_key_title)
                )
        return set(titles)

    def get_function_keys(self, name: str) -> list:
        """Return the keys for the named function."""
        try:
            return self.registered_commands[name]
        except KeyError as error:
            raise KeyError('Unbound key function: {}'.format(name)) from error

    def register_named_key_function(
        self, name: str, default_bindings: list[str]
    ) -> None:
        self.registered_commands[name] = default_bindings

    def register_keybinding(
        self, name: str, key_bindings: KeyBindings, **kwargs
    ) -> Callable:
        """Apply registered keys for the given named function."""

        def decorator(handler: Callable) -> Callable:
            "`handler` is a callable or Binding."
            for keys in self.get_function_keys(name):
                key_bindings.add(*keys.split(' '), **kwargs)(handler)
            return handler

        return decorator

    @property
    def snippets(self) -> dict:
        return self._config.get('snippets', {})

    @property
    def user_snippets(self) -> dict:
        return self._config.get('user_snippets', {})

    def snippet_completions(self) -> list[CodeSnippet]:
        if self._snippet_completions:
            return self._snippet_completions

        all_snippets: list[CodeSnippet] = []

        def previous_description() -> str | None:
            if not all_snippets:
                return None
            return all_snippets[-1].description

        for title, value in self.user_snippets.items():
            all_snippets.append(
                CodeSnippet.from_yaml(title, value, previous_description())
            )
        for title, value in self.snippets.items():
            all_snippets.append(
                CodeSnippet.from_yaml(title, value, previous_description())
            )

        self._snippet_completions = all_snippets

        return self._snippet_completions
