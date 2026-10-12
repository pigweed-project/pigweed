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
"""Defines arguments for the pw command."""

import argparse
from dataclasses import dataclass
from enum import Enum
from functools import cached_property
import logging
import os
from pathlib import Path
import sys
from typing import Any, NoReturn

from pw_cli import argument_types
from pw_cli.branding import banner
import pw_cli.env

_HELP_HEADER = '''The Pigweed command line interface (CLI).

Example uses:
    pw logdemo
    pw --loglevel debug watch -C out
'''


def parse_args() -> argparse.Namespace:
    return arg_parser().parse_args()


class ShellCompletionFormat(Enum):
    """Supported shell tab completion modes."""

    BASH = 'bash'
    FISH = 'fish'
    ZSH = 'zsh'


@dataclass(frozen=True)
class ShellCompletion:
    """Transforms argparse actions into bash, fish, zsh shell completions."""

    action: argparse.Action
    parser: argparse.ArgumentParser

    @property
    def option_strings(self) -> list[str]:
        return list(self.action.option_strings)

    @cached_property
    def help(self) -> str:
        return self.parser._get_formatter()._expand_help(  # pylint: disable=protected-access
            self.action
        )

    @property
    def choices(self) -> list[str]:
        return list(self.action.choices) if self.action.choices else []

    @property
    def flag(self) -> bool:
        return self.action.nargs == 0

    @property
    def default(self) -> Any:
        return self.action.default

    def bash_option(self, text: str) -> list[str]:
        result: list[str] = []
        for option_str in self.option_strings:
            if option_str.startswith(text):
                result.append(option_str)
        return result

    def zsh_option(self, text: str) -> list[str]:
        result: list[str] = []
        for option_str in self.option_strings:
            if not option_str.startswith(text):
                continue

            short_and_long_opts = ' '.join(self.option_strings)
            # '(-h --help)-h[Display help message and exit]'
            # '(-h --help)--help[Display help message and exit]'
            help_text = self.help if self.help else ''

            state_str = ''
            if not self.flag:
                state_str = ': :->' + option_str

            result.append(
                f'({short_and_long_opts}){option_str}[{help_text}]'
                f'{state_str}'
            )
        return result

    def fish_option(self, text: str) -> list[str]:
        result: list[str] = []
        for option_str in self.option_strings:
            if not option_str.startswith(text):
                continue

            output: list[str] = []
            if option_str.startswith('--'):
                output.append(f'--long-option\t{option_str.lstrip("-")}')
            elif option_str.startswith('-'):
                output.append(f'--short-option\t{option_str.lstrip("-")}')

            if self.choices:
                choice_str = " ".join(self.choices)
                output.append('--exclusive')
                output.append('--arguments')
                output.append(f'(string split " " "{choice_str}")')
            elif self.action.type == Path:
                output.append('--require-parameter')
                output.append('--force-files')

            if self.help:
                output.append(f'--description\t"{self.help}"')

            result.append('\t'.join(output))
        return result


def get_options_and_help(
    parser: argparse.ArgumentParser,
) -> list[ShellCompletion]:
    return list(
        ShellCompletion(action=action, parser=parser)
        for action in parser._actions  # pylint: disable=protected-access
    )


def print_completions_for_option(
    parser: argparse.ArgumentParser,
    text: str = '',
    tab_completion_format: str = ShellCompletionFormat.BASH.value,
) -> None:
    matched_lines: list[str] = []
    for completion in get_options_and_help(parser):
        if tab_completion_format == ShellCompletionFormat.ZSH.value:
            matched_lines.extend(completion.zsh_option(text))
        if tab_completion_format == ShellCompletionFormat.FISH.value:
            matched_lines.extend(completion.fish_option(text))
        else:
            matched_lines.extend(completion.bash_option(text))

    for line in matched_lines:
        print(line)


def is_interactive_terminal() -> bool:
    """Returns True if stdout and stderr are human-interactive terminals."""
    # If not connected to a TTY on both stdout and stderr, this is a pipe,
    # redirect, subshell, or non-interactive script.
    if not (sys.stderr.isatty() and sys.stdout.isatty()):
        return False

    # Agents, Emacs compile buffers, and headless runners typically use
    # TERM=dumb.
    if os.environ.get('TERM', '') in ('dumb', ''):
        return False

    # Common CI and AI agent environment variables.
    agent_or_ci_vars = (
        'AI_AGENT',
        'ANTIGRAVITY_TRAJECTORY_ID',
        'CI',
        'CLAUDECODE',
        'CONTINUOUS_INTEGRATION',
        'CURSOR_AGENT',
        'INVOKER_INFO_NAME',  # Jetski / Antigravity
        'JETSKI',
    )
    if any(var in os.environ for var in agent_or_ci_vars):
        return False

    return True


def print_banner() -> None:
    """Prints the PIGWEED (or project specific) banner to stderr."""
    print(banner() + '\n', file=sys.stderr)


def maybe_print_banner() -> None:
    """Prints the banner to stderr if enabled and in an interactive terminal."""
    parsed_env = pw_cli.env.pigweed_environment()
    if (
        parsed_env.PW_ENVSETUP_NO_BANNER
        or parsed_env.PW_ENVSETUP_QUIET
        or not is_interactive_terminal()
    ):
        return
    print_banner()


class _ArgumentParserWithBanner(argparse.ArgumentParser):
    """Parser that prints the Pigweed banner when there are parsing errors."""

    def error(self, message: str) -> NoReturn:
        maybe_print_banner()
        self.print_usage(sys.stderr)
        self.exit(2, f'{self.prog}: error: {message}\n')


def add_tab_complete_arguments(
    parser: argparse.ArgumentParser,
) -> argparse.ArgumentParser:
    parser.add_argument(
        '--tab-complete-option',
        nargs='?',
        help='Print tab completions for the supplied option text.',
    )
    parser.add_argument(
        '--tab-complete-format',
        choices=list(shell.value for shell in ShellCompletionFormat),
        default='bash',
        help='Output format for tab completion results.',
    )
    return parser


def arg_parser() -> argparse.ArgumentParser:
    """Creates an argument parser for the pw command."""
    argparser = _ArgumentParserWithBanner(
        prog='pw',
        add_help=False,
        description=_HELP_HEADER,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # Do not use the built-in help argument so that displaying the help info can
    # be deferred until the pw plugins have been registered.
    argparser.add_argument(
        '-h',
        '--help',
        action='store_true',
        help='Display this help message and exit',
    )
    argparser.add_argument(
        '-C',
        '--directory',
        type=argument_types.directory,
        default=Path.cwd(),
        help='Change to this directory before doing anything',
    )
    argparser.add_argument(
        '-l',
        '--loglevel',
        type=argument_types.log_level,
        default=logging.INFO,
        help='Set the log level (debug, info, warning, error, critical)',
    )
    argparser.add_argument(
        '--debug-log',
        type=Path,
        help=(
            'Additionally log to this file at debug level; does not affect '
            'terminal output'
        ),
    )
    argparser.add_argument(
        '--banner',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Whether to print the Pigweed banner',
    )
    argparser.add_argument(
        '--tab-complete-command',
        nargs='?',
        help='Print tab completions for the supplied command text.',
    )

    default_analytics = None
    if pw_cli.env.pigweed_environment().PW_DISABLE_CLI_ANALYTICS:
        default_analytics = False
    argparser.add_argument(
        '--analytics',
        action='store_true',
        default=default_analytics,
        help='Temporarily enable analytics collection.',
    )
    argparser.add_argument(
        '--no-analytics',
        action='store_false',
        dest='analytics',
        help='Temporarily disable analytics collection.',
    )

    return add_tab_complete_arguments(argparser)
