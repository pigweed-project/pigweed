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
"""Utilities for authoring busybox-style multitools."""

import abc
import argparse
import logging
import os
import sys
from typing import Mapping, NoReturn, Sequence

from pw_cli_analytics import analytics as cli_analytics
import pw_cli.env
import pw_cli.log
import pw_cli.plugins
from pw_cli import arguments

_LOG = logging.getLogger(__name__)


class MultitoolPlugin(abc.ABC):
    """A single runnable tool in a multitool-style hub."""

    @abc.abstractmethod
    def name(self) -> str:
        """The unique name of this tool."""

    @abc.abstractmethod
    def help(self) -> str:
        """A string that describes what this tool does."""

    @abc.abstractmethod
    def run(self, plugin_args: Sequence[str]) -> int:
        """Runs the underlying tool."""


class MultitoolCli:
    """A class that simplifies declaring a multitool hub."""

    def __init__(self):
        self._top_level_args_added = False

    @abc.abstractmethod
    def plugins(self, args: argparse.Namespace) -> Sequence[MultitoolPlugin]:
        """Populates a list of available plugins."""

    def add_arguments(
        self,
        parser: argparse.ArgumentParser,  # pylint: disable=unused-argument
    ) -> None:
        """Extend arguments with application-specific options."""
        # Make sure all super-class arguments have been added.
        self._top_level_args_added = True

    @staticmethod
    def _plugin_help(plugins: Mapping[str, MultitoolPlugin]) -> str:
        """Creates a unified help string for all plugins."""
        width = (
            max(len(name) for name in (plugins.keys())) + 1 if plugins else 1
        )
        subcommands_pretty = '\n'.join(
            f'  {subcommand:{width}} {plugin.help()}'
            for subcommand, plugin in sorted(plugins.items())
        )
        return f'supported plugins:\n{subcommands_pretty}'

    @staticmethod
    def _format_help(
        parser: argparse.ArgumentParser, plugins: Mapping[str, MultitoolPlugin]
    ):
        return '\n'.join(
            (
                parser.format_help(),
                MultitoolCli._plugin_help(plugins),
            )
        )

    def main(self) -> NoReturn:
        """Entry point for the CLI tool."""

        argv_copy = sys.argv[:]
        parser = arguments.arg_parser()
        self.add_arguments(parser)
        assert self._top_level_args_added, (
            'Parent multitool args are missing, did you forget to call '
            'super().add_arguments()?'
        )

        parser.add_argument(
            'command',
            nargs='?',
            help='Which command to run; see supported commands below',
        )
        parser.add_argument(
            'plugin_args',
            metavar='...',
            nargs=argparse.REMAINDER,
            help='Remaining arguments are forwarded to the command',
        )

        args = parser.parse_args()

        pw_cli.log.install(level=args.loglevel, debug_log=args.debug_log)

        # Print the banner unless --no-banner or a tab completion arg is
        # present.
        # Note: args.tab_complete_{command,option} may be the empty string
        # '' so check for None instead.
        if (
            args.banner
            and args.tab_complete_option is None
            and args.tab_complete_command is None
        ):
            arguments.maybe_print_banner()

        _LOG.debug('Executing the pw command from %s', args.directory)
        os.chdir(args.directory)

        plugin_map: dict[str, MultitoolPlugin] = {
            plugin.name(): plugin for plugin in self.plugins(args)
        }

        if args.tab_complete_option is not None:
            arguments.print_completions_for_option(
                parser,
                text=args.tab_complete_option,
                tab_completion_format=args.tab_complete_format,
            )
            sys.exit(0)

        if args.tab_complete_command is not None:
            for name, plugin in sorted(plugin_map.items()):
                if name.startswith(args.tab_complete_command):
                    if args.tab_complete_format in ('fish', 'zsh'):
                        print(':'.join([name, plugin.help()]))
                    else:
                        print(name)
            sys.exit(0)

        if args.help or args.command is None:
            print(self._format_help(parser, plugin_map), file=sys.stderr)
            sys.exit(0)

        if args.analytics is None:
            args.analytics = True

        env = pw_cli.env.pigweed_environment()
        if env.PW_DISABLE_CLI_ANALYTICS:
            args.analytics = False

        analytics: cli_analytics.Analytics | None = None
        analytics_state = cli_analytics.State.UNKNOWN
        if args.analytics and args.command != 'cli-analytics':
            # If there are no analytics settings we need to initialize them.
            # Don't send telemetry out on the initial run.
            analytics_state = cli_analytics.initialize()
            if analytics_state != cli_analytics.State.NEWLY_INITIALIZED:
                analytics = cli_analytics.Analytics(argv_copy, args)
                try:
                    analytics.begin()
                except Exception:  # pylint: disable=broad-except
                    analytics = None

        status = -1
        try:
            status = plugin_map[args.command].run(args.plugin_args)
        except (pw_cli.plugins.Error, KeyError) as err:
            _LOG.critical('Cannot run command %s.', args.command)
            _LOG.critical('%s', err)
            status = 2
        finally:
            try:
                if analytics:
                    analytics.end(status=status)
                cli_analytics.finalize()
            except Exception:  # pylint: disable=broad-except
                pass

        sys.exit(status)
