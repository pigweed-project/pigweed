#!/usr/bin/env python
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
"""Watch files for changes and rebuild.

Run arbitrary commands or invoke build systems (Ninja, Bazel and make) on one or
more build directories whenever source files change.

Examples:

  # Build the default target in out/ using ninja.
  pw watch -C out

  # Build python.lint and stm32f429i targets in out/ using ninja.
  pw watch python.lint stm32f429i

  # Build pw_run_tests.modules in the out/cmake directory
  pw watch -C out/cmake pw_run_tests.modules

  # Build the default target in out/ and pw_apps in out/cmake
  pw watch -C out -C out/cmake pw_apps

  # Build python.tests in out/ and pw_apps in out/cmake/
  pw watch python.tests -C out/cmake pw_apps

  # Run 'bazel build' and 'bazel test' on the target '//...' in out/bazel/
  pw watch -C out/bazel '//...'
  --build-system-command out/bazel 'bazel build'
  --build-system-command out/bazel 'bazel test'
"""

import argparse
import concurrent.futures
import errno
import http.server
import logging
import os
from pathlib import Path
import socketserver
import sys
import threading
from threading import Thread
from typing import Callable, Sequence

from watchdog.events import FileSystemEventHandler

from prompt_toolkit import prompt

from pw_build.build_recipe import BuildRecipe, create_build_recipes
from pw_build.project_builder import (
    ProjectBuilder,
    execute_command_pure,
    execute_command_with_logging,
    ASCII_CHARSET,
    EMOJI_CHARSET,
)
from pw_build.project_builder_context import get_project_builder_context
import pw_cli.branding
import pw_cli.env
import pw_cli.log
import pw_console.python_logging

from pw_watch.argparser import (
    WATCH_PATTERN_DELIMITER,
    WATCH_PATTERN_STRING,
    add_parser_arguments,
)
from pw_watch.debounce import DebouncedFunction, Debouncer
from pw_watch import common
from pw_watch.watch_app import WatchAppPrefs, WatchApp

_LOG = logging.getLogger('pw_build.watch')

_FULLSCREEN_STATUS_COLUMN_WIDTH = 10

BUILDER_CONTEXT = get_project_builder_context()


def _log_event(event_description: str) -> None:
    if BUILDER_CONTEXT.using_progress_bars():
        _LOG.warning('Event while running: %s', event_description)
    else:
        print('\n')
        _LOG.warning('Event while running: %s', event_description)
        print('')


class PigweedBuildWatcher(FileSystemEventHandler, DebouncedFunction):
    """Process filesystem events and launch builds if necessary."""

    # pylint: disable=too-many-instance-attributes
    def __init__(  # pylint: disable=too-many-arguments
        self,
        project_builder: ProjectBuilder,
        patterns: Sequence[str] = (),
        ignore_patterns: Sequence[str] = (),
        restart: bool = True,
        fullscreen: bool = False,
        banners: bool = True,
        use_logfile: bool = False,
        separate_logfiles: bool = False,
        parallel_workers: int = 1,
    ):
        super().__init__()

        self.banners = banners
        self.current_build_step = ''
        self.current_build_percent = 0.0
        self.current_build_errors = 0
        self.patterns = patterns
        self.ignore_patterns = ignore_patterns
        self.project_builder = project_builder
        self.parallel_workers = parallel_workers

        self.restart_on_changes = restart
        self.fullscreen_enabled = fullscreen
        self.watch_app: WatchApp | None = None

        self.use_logfile = use_logfile
        self.separate_logfiles = separate_logfiles
        if self.parallel_workers > 1:
            self.separate_logfiles = True

        self.debouncer = Debouncer(self, log_event=_log_event)

        # Track state of a build. These need to be members instead of locals
        # due to the split between dispatch(), run(), and on_complete().
        self.matching_path: Path | None = None

        if (
            not self.fullscreen_enabled
            and not self.project_builder.should_use_progress_bars()
        ):
            self.wait_for_keypress_thread = threading.Thread(
                None, self._wait_for_enter
            )
            self.wait_for_keypress_thread.start()

        if self.fullscreen_enabled:
            BUILDER_CONTEXT.using_fullscreen = True

    def rebuild(self):
        """Rebuild command triggered from watch app."""
        self.debouncer.press('Manual build requested')

    def _wait_for_enter(self) -> None:
        try:
            while True:
                _ = prompt('')
                self.rebuild()
        # Ctrl-C on Unix generates KeyboardInterrupt
        # Ctrl-Z on Windows generates EOFError
        except (KeyboardInterrupt, EOFError):
            # Force stop any running ninja builds.
            _exit_due_to_interrupt()

    def dispatch(self, event) -> None:
        path = common.handle_watchdog_event(
            event, self.patterns, self.ignore_patterns
        )
        if path is not None:
            self._handle_matched_event(path)

    def _handle_matched_event(self, matching_path: Path) -> None:
        if self.matching_path is None:
            self.matching_path = matching_path

        log_message = f'File change detected: {os.path.relpath(matching_path)}'
        if self.restart_on_changes:
            if self.fullscreen_enabled and self.watch_app:
                self.watch_app.clear_log_panes()
            self.debouncer.press(f'{log_message} Triggering build...')
        else:
            _LOG.info('%s ; not rebuilding', log_message)

    def _clear_screen(self) -> None:
        if self.fullscreen_enabled:
            return
        if self.project_builder.should_use_progress_bars():
            BUILDER_CONTEXT.clear_progress_scrollback()
            return
        print('\033c', end='')  # TODO(pwbug/38): Not Windows compatible.
        sys.stdout.flush()

    # Implementation of DebouncedFunction.run()
    #
    # Note: This will run on the timer thread created by the Debouncer, rather
    # than on the main thread that's watching file events. This enables the
    # watcher to continue receiving file change events during a build.
    def run(self) -> None:
        """Run all the builds and capture pass/fail for each."""

        # Clear the screen and show a banner indicating the build is starting.
        self._clear_screen()

        if self.banners:
            for line in pw_cli.branding.banner().splitlines():
                _LOG.info(line)
        if self.fullscreen_enabled:
            _LOG.info(
                self.project_builder.color.green(
                    'Watching for changes. Ctrl-d to exit; enter to rebuild'
                )
            )
        else:
            _LOG.info(
                self.project_builder.color.green(
                    'Watching for changes. Ctrl-C to exit; enter to rebuild'
                )
            )
        if self.matching_path:
            _LOG.info('')
            _LOG.info('Change detected: %s', self.matching_path)

        num_builds = len(self.project_builder)
        _LOG.info('Starting build with %d recipes', num_builds)

        if self.project_builder.default_logfile:
            _LOG.info(
                '%s %s',
                self.project_builder.color.blue('Root logfile:'),
                self.project_builder.default_logfile.resolve(),
            )

        env = os.environ.copy()
        if self.project_builder.colors:
            # Force colors in Pigweed subcommands run through the watcher.
            env['PW_USE_COLOR'] = '1'
            # Force Ninja to output ANSI colors
            env['CLICOLOR_FORCE'] = '1'

        # Reset status
        BUILDER_CONTEXT.set_project_builder(self.project_builder)
        BUILDER_CONTEXT.set_enter_callback(self.rebuild)
        BUILDER_CONTEXT.set_building()

        for cfg in self.project_builder:
            cfg.reset_status()

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self.parallel_workers
        ) as executor:
            futures = []
            if (
                not self.fullscreen_enabled
                and self.project_builder.should_use_progress_bars()
            ):
                BUILDER_CONTEXT.add_progress_bars()

            for i, cfg in enumerate(self.project_builder, start=1):
                futures.append(executor.submit(self.run_recipe, i, cfg, env))

            for future in concurrent.futures.as_completed(futures):
                future.result()

        BUILDER_CONTEXT.set_idle()

    def run_recipe(self, index: int, cfg: BuildRecipe, env) -> None:
        if BUILDER_CONTEXT.interrupted():
            return
        if not cfg.enabled:
            return

        num_builds = len(self.project_builder)
        index_message = f'[{index}/{num_builds}]'

        self.project_builder.log_build_recipe_start(
            index_message, cfg, logger=_LOG
        )

        self.project_builder.run_build(
            cfg,
            env,
            index_message=index_message,
        )

        self.project_builder.log_build_recipe_finish(
            index_message,
            cfg,
            logger=_LOG,
        )

    def execute_command(
        self,
        command: list,
        env: dict,
        recipe: BuildRecipe,
        # pylint: disable=unused-argument
        *args,
        **kwargs,
        # pylint: enable=unused-argument
    ) -> bool:
        """Runs a command with a blank before/after for visual separation."""
        if self.fullscreen_enabled:
            return self._execute_command_watch_app(command, env, recipe)

        if self.separate_logfiles:
            return execute_command_with_logging(
                command, env, recipe, logger=recipe.log
            )

        if self.use_logfile:
            return execute_command_with_logging(
                command, env, recipe, logger=_LOG
            )

        return execute_command_pure(command, env, recipe)

    def _execute_command_watch_app(
        self,
        command: list,
        env: dict,
        recipe: BuildRecipe,
    ) -> bool:
        """Runs a command with and outputs the logs."""
        if not self.watch_app:
            return False

        self.watch_app.redraw_ui()

        def new_line_callback(recipe: BuildRecipe) -> None:
            self.current_build_step = recipe.status.current_step
            self.current_build_percent = recipe.status.percent
            self.current_build_errors = recipe.status.error_count

            if self.watch_app:
                self.watch_app.logs_redraw()

        desired_logger = _LOG
        if self.separate_logfiles:
            desired_logger = recipe.log

        result = execute_command_with_logging(
            command,
            env,
            recipe,
            logger=desired_logger,
            line_processed_callback=new_line_callback,
        )

        self.watch_app.redraw_ui()

        return result

    # Implementation of DebouncedFunction.cancel()
    def cancel(self) -> bool:
        if self.restart_on_changes:
            BUILDER_CONTEXT.restart_flag = True
            BUILDER_CONTEXT.terminate_and_wait()
            return True

        return False

    # Implementation of DebouncedFunction.on_complete()
    def on_complete(self, cancelled: bool = False) -> None:
        # First, use the standard logging facilities to report build status.
        if cancelled:
            _LOG.info('Build stopped.')
        elif BUILDER_CONTEXT.interrupted():
            pass  # Don't print anything.
        elif all(
            recipe.status.passed()
            for recipe in self.project_builder
            if recipe.enabled
        ):
            _LOG.info('Finished; all successful')
        else:
            _LOG.info('Finished; some builds failed')

        # For non-fullscreen pw watch
        if (
            not self.fullscreen_enabled
            and not self.project_builder.should_use_progress_bars()
        ):
            # Show a more distinct colored banner.
            self.project_builder.print_build_summary(
                cancelled=cancelled, logger=_LOG
            )
        self.project_builder.print_pass_fail_banner(
            cancelled=cancelled, logger=_LOG
        )

        if self.watch_app:
            self.watch_app.redraw_ui()
        self.matching_path = None

    # Implementation of DebouncedFunction.on_keyboard_interrupt()
    def on_keyboard_interrupt(self) -> None:
        _exit_due_to_interrupt()


def _exit_due_to_interrupt() -> None:
    # To keep the log lines aligned with each other in the presence of
    # a '^C' from the keyboard interrupt, add a newline before the log.
    print('')
    _LOG.info('Got Ctrl-C; exiting...')
    BUILDER_CONTEXT.ctrl_c_interrupt()


def _exit_due_to_inotify_watch_limit():
    common.log_inotify_watch_limit_reached()
    common.exit_immediately(1)


def _exit_due_to_inotify_instance_limit():
    common.log_inotify_instance_limit_reached()
    common.exit_immediately(1)


def _simple_docs_server(
    address: str, port: int, path: Path
) -> Callable[[], None]:
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(path), **kwargs)

        # Disable logs to stdout
        def log_message(
            self, format: str, *args  # pylint: disable=redefined-builtin
        ) -> None:
            return

    def simple_http_server_thread():
        with socketserver.TCPServer((address, port), Handler) as httpd:
            httpd.serve_forever()

    return simple_http_server_thread


def _serve_docs(
    build_dir: Path,
    docs_path: Path,
    address: str = '127.0.0.1',
    port: int = 8000,
) -> None:
    address = '127.0.0.1'
    docs_path = build_dir.joinpath(docs_path.joinpath('html'))
    server_thread = _simple_docs_server(address, port, docs_path)
    _LOG.info('Serving docs at http://%s:%d', address, port)

    # Spin up server in a new thread since it blocks
    threading.Thread(None, server_thread, 'pw_docs_server').start()


def watch_logging_init(log_level: int, fullscreen: bool, colors: bool) -> None:
    # Logging setup
    if not fullscreen:
        pw_cli.log.install(
            level=log_level,
            use_color=colors,
            hide_timestamp=False,
        )
        return

    watch_logfile = pw_console.python_logging.create_temp_log_file(
        prefix=__package__
    )

    pw_cli.log.install(
        level=logging.DEBUG,
        use_color=colors,
        hide_timestamp=False,
        log_file=watch_logfile,
    )


def watch_setup(  # pylint: disable=too-many-locals
    project_builder: ProjectBuilder,
    # NOTE: The following args should have defaults matching argparse. This
    # allows use of watch_setup by other project build scripts.
    patterns: str = WATCH_PATTERN_STRING,
    ignore_patterns_string: str = '',
    exclude_list: list[Path] | None = None,
    restart: bool = True,
    serve_docs: bool = False,
    serve_docs_port: int = 8000,
    serve_docs_path: Path = Path('docs/gen/docs'),
    fullscreen: bool = False,
    banners: bool = True,
    logfile: Path | None = None,
    separate_logfiles: bool = False,
    parallel: bool = False,
    parallel_workers: int = 0,
    # pylint: disable=unused-argument
    default_build_targets: list[str] | None = None,
    build_directories: list[str] | None = None,
    build_system_commands: list[str] | None = None,
    run_command: list[str] | None = None,
    jobs: int | None = None,
    keep_going: bool = False,
    colors: bool = True,
    debug_logging: bool = False,
    source_path: Path | None = None,
    default_build_system: str | None = None,
    # pylint: enable=unused-argument
    # pylint: disable=too-many-arguments
) -> tuple[PigweedBuildWatcher, list[Path]]:
    """Watches files and runs Ninja commands when they change."""
    watch_logging_init(
        log_level=project_builder.default_log_level,
        fullscreen=fullscreen,
        colors=colors,
    )

    # Update the project_builder log formatters since pw_cli.log.install may
    # have changed it.
    project_builder.apply_root_log_formatting()

    if project_builder.should_use_progress_bars():
        project_builder.use_stdout_proxy()

    _LOG.info('Starting Pigweed build watcher')

    build_recipes = project_builder.build_recipes

    if source_path is None:
        source_path = pw_cli.env.project_root()

    # Preset exclude list for pigweed directory.
    if not exclude_list:
        exclude_list = []
    exclude_list += common.get_common_excludes(source_path)

    # Add build directories to the exclude list if they are not already ignored.
    for build_dir in list(
        cfg.build_dir.resolve()
        for cfg in build_recipes
        if isinstance(cfg.build_dir, Path)
    ):
        if not any(
            # Check if build_dir.is_relative_to(excluded_dir)
            build_dir == excluded_dir or excluded_dir in build_dir.parents
            for excluded_dir in exclude_list
        ):
            exclude_list.append(build_dir)

    for i, build_recipe in enumerate(build_recipes, start=1):
        _LOG.info('Will build [%d/%d]: %s', i, len(build_recipes), build_recipe)

    _LOG.debug('Patterns: %s', patterns)

    for excluded_dir in exclude_list:
        _LOG.debug('exclude-list: %s', excluded_dir)

    if serve_docs:
        _serve_docs(
            build_recipes[0].build_dir, serve_docs_path, port=serve_docs_port
        )

    # Ignore the user-specified patterns.
    ignore_patterns = (
        ignore_patterns_string.split(WATCH_PATTERN_DELIMITER)
        if ignore_patterns_string
        else []
    )

    # Add project_builder logfiles to ignore_patterns
    if project_builder.default_logfile:
        ignore_patterns.append(str(project_builder.default_logfile))
    if project_builder.separate_build_file_logging:
        for recipe in project_builder:
            if recipe.logfile:
                ignore_patterns.append(str(recipe.logfile))

    workers = 1
    if parallel:
        # If parallel is requested and parallel_workers is set to 0 run all
        # recipes in parallel. That is, use the number of recipes as the worker
        # count.
        if parallel_workers == 0:
            workers = len(project_builder)
        else:
            workers = parallel_workers

    event_handler = PigweedBuildWatcher(
        project_builder=project_builder,
        patterns=patterns.split(WATCH_PATTERN_DELIMITER),
        ignore_patterns=ignore_patterns,
        restart=restart,
        fullscreen=fullscreen,
        banners=banners,
        use_logfile=bool(logfile),
        separate_logfiles=separate_logfiles,
        parallel_workers=workers,
    )

    project_builder.execute_command = event_handler.execute_command

    return event_handler, exclude_list


def watch(
    event_handler: PigweedBuildWatcher,
    exclude_list: list[Path],
    watch_file_path: Path = Path.cwd(),
):
    """Watches files and runs Ninja commands when they change."""
    if event_handler.project_builder.source_path:
        watch_file_path = event_handler.project_builder.source_path

    try:
        wait = common.watch(watch_file_path, exclude_list, event_handler)
        event_handler.debouncer.press('Triggering initial build...')
        wait()
    # Ctrl-C on Unix generates KeyboardInterrupt
    # Ctrl-Z on Windows generates EOFError
    except (KeyboardInterrupt, EOFError):
        _exit_due_to_interrupt()
    except OSError as err:
        if err.args[0] == common.ERRNO_INOTIFY_LIMIT_REACHED:
            if event_handler.watch_app:
                event_handler.watch_app.exit(
                    log_after_shutdown=common.log_inotify_watch_limit_reached
                )
            elif event_handler.project_builder.should_use_progress_bars():
                BUILDER_CONTEXT.exit(
                    log_after_shutdown=common.log_inotify_watch_limit_reached
                )
            else:
                _exit_due_to_inotify_watch_limit()
        if err.errno == errno.EMFILE:
            if event_handler.watch_app:
                event_handler.watch_app.exit(
                    log_after_shutdown=common.log_inotify_instance_limit_reached
                )
            elif event_handler.project_builder.should_use_progress_bars():
                BUILDER_CONTEXT.exit(
                    log_after_shutdown=common.log_inotify_instance_limit_reached
                )
            else:
                _exit_due_to_inotify_instance_limit()
        raise err


def run_watch(
    event_handler: PigweedBuildWatcher,
    exclude_list: list[Path],
    prefs: WatchAppPrefs | None = None,
    fullscreen: bool = False,
) -> None:
    """Start pw_watch."""
    if not prefs:
        prefs = WatchAppPrefs(load_argparse_arguments=add_parser_arguments)

    if fullscreen:
        watch_thread = Thread(
            target=watch,
            args=(event_handler, exclude_list),
            daemon=True,
        )
        watch_thread.start()
        watch_app = WatchApp(
            event_handler=event_handler,
            prefs=prefs,
        )

        event_handler.watch_app = watch_app
        watch_app.run()

    else:
        watch(event_handler, exclude_list)


def get_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser = add_parser_arguments(parser)
    return parser


def main() -> int:
    """Watch files for changes and rebuild."""
    parser = get_parser()
    args = parser.parse_args()

    prefs = WatchAppPrefs(load_argparse_arguments=add_parser_arguments)
    prefs.apply_command_line_args(args)
    build_recipes = create_build_recipes(prefs)

    env = pw_cli.env.pigweed_environment()
    if env.PW_EMOJI:
        charset = EMOJI_CHARSET
    else:
        charset = ASCII_CHARSET

    # Force separate-logfiles for split window panes if running in parallel.
    separate_logfiles = args.separate_logfiles
    if args.parallel:
        separate_logfiles = True

    def _recipe_abort(*args) -> None:
        _LOG.critical(*args)

    project_builder = ProjectBuilder(
        build_recipes=build_recipes,
        jobs=args.jobs,
        banners=args.banners,
        keep_going=args.keep_going,
        colors=args.colors,
        charset=charset,
        separate_build_file_logging=separate_logfiles,
        root_logfile=args.logfile,
        root_logger=_LOG,
        log_level=logging.DEBUG if args.debug_logging else logging.INFO,
        abort_callback=_recipe_abort,
        source_path=args.source_path,
        dry_run=args.dry_run,
    )

    event_handler, exclude_list = watch_setup(project_builder, **vars(args))

    run_watch(
        event_handler,
        exclude_list,
        prefs=prefs,
        fullscreen=args.fullscreen,
    )

    return 0


if __name__ == '__main__':
    main()
