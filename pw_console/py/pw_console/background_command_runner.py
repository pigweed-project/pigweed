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
"""Background command runner classes."""

import concurrent.futures
import logging
import os
import shlex
import subprocess
import threading
from typing import (
    Callable,
    TYPE_CHECKING,
)

from pw_console.log_pane import LogPane
from pw_console.background_command_log_parsers import get_command_log_parser
from pw_console.text_formatting import (
    strip_incompatible_ansi,
    unterminated_ansi_color_codes,
)

if TYPE_CHECKING:
    from pw_console.console_app import ConsoleApp


class BackgroundTask:
    """Class to hold a single background task."""

    def __init__(
        self,
        command: str,
        logger_name: str,
        line_processed_callback: Callable[[], None] | None = None,
        process_log: Callable[[logging.Logger, str], None] | None = None,
        log_pane_title: str | None = None,
    ) -> None:
        self.command = shlex.split(command)
        self.logger = logging.getLogger(logger_name)
        self.logger.propagate = False
        self.line_processed_callback = line_processed_callback
        if not log_pane_title:
            self.log_pane_title = ' '.join(self.command)
        else:
            self.log_pane_title = log_pane_title

        self._stop_signal = threading.Event()
        self.returncode: int | None = None
        self.proc: subprocess.Popen | None = None
        self.thread: threading.Thread | None = None
        self.process_log = process_log

    def _execute_command_with_logging(self) -> None:
        self.returncode = None
        env = os.environ.copy()
        # Force colors in Pigweed subcommands and some terminal apps.
        env['PW_USE_COLOR'] = '1'
        env['CLICOLOR_FORCE'] = '1'

        # Create the process_output function depending on if a custom
        # process_log function is available.
        if self.process_log is not None:

            def _process_output(output: str) -> None:
                self.process_log(self.logger, output)  # type: ignore

        else:

            def _process_output(output: str) -> None:
                # Use logger.info on the message by itself.
                self.logger.info(output)

        # Launch the process
        try:
            with subprocess.Popen(
                self.command,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                errors='replace',
            ) as proc:
                self.proc = proc

                # Track color codes that span multiple lines.
                previous_colors = None

                # While the process is running
                while self.returncode is None:
                    # Check for exit signal
                    if self._stop_signal.is_set():
                        break

                    # Get the next line
                    output = ''
                    if proc.stdout:
                        output = strip_incompatible_ansi(
                            proc.stdout.readline().rstrip()
                        )

                    if output:
                        # Add previous colors if any exist. This lets colors
                        # span multiple log lines.
                        if previous_colors:
                            output = ''.join(previous_colors) + output

                        # Check for untermintated color codes
                        previous_colors = unterminated_ansi_color_codes(output)
                        if previous_colors:
                            # There were colors that weren't cleared, append the
                            # reset code
                            output += '\x1b[0m'

                        # Log the line
                        _process_output(output)
                        # Update the UI
                        if self.line_processed_callback:
                            self.line_processed_callback()
                    else:
                        # No output, check the return code.
                        self.returncode = proc.poll()
                        continue

            # Exit signal was raised at this point. Check if the process has
            # ended.
            self.returncode = proc.poll()
            # If still running, terminate it.
            if not self.returncode:
                self._wait_for_terminate_then_kill()

        # If the subprocess can't run the command:
        except FileNotFoundError as exception:
            self.logger.error('%s', exception)

    def is_running(self) -> bool:
        if self.proc and self.proc.returncode is None:
            return True
        return False

    def stop_process(self) -> bool:
        """Entry point for the stop process menu option."""
        self._stop_signal.set()
        # return true to close the menu
        return True

    def _wait_for_terminate_then_kill(self) -> None:
        """Wait for a process to end, then kill it if the timeout expires."""
        if not self.proc:
            return
        if self.proc.returncode is not None:
            return

        proc_command = self.proc.args
        if isinstance(self.proc.args, list):
            proc_command = ' '.join(str(a) for a in self.proc.args)
        try:
            self.logger.debug('Stopping %s', proc_command)
            self.proc.terminate()
            self.returncode = self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.logger.debug('Killing %s', proc_command)
            self.proc.kill()

    def restart_process(self) -> bool:
        if self.is_running():
            self.stop_process()
        self.start_command_thread()
        return True

    def start_command_thread(self) -> None:
        self.thread = threading.Thread(
            target=self._execute_command_with_logging, args=(), daemon=True
        )
        self.thread.start()


class BackgroundCommandRunner:
    """Class that manages running tasks and creating their log panes."""

    def __init__(self, application: 'ConsoleApp'):
        # Parent pw_console application
        self.application = application

        self.tasks: list[BackgroundTask] = []

    def _line_processed_callback(self) -> None:
        self.application.logs_redraw()

    def execute_new_command(
        self,
        command: str,
        process_log: Callable[[logging.Logger, str], None] | None = None,
        process_log_name: str | None = None,
        log_pane_title: str | None = None,
    ) -> LogPane:
        task_number = len(self.tasks)
        logger_name = f'pw_console.background_command.{task_number}'
        if process_log_name and not process_log:
            process_log = get_command_log_parser(process_log_name)

        background_task = BackgroundTask(
            command,
            logger_name=logger_name,
            line_processed_callback=self._line_processed_callback,
            process_log=process_log,
        )
        title = log_pane_title
        if not title:
            title = background_task.log_pane_title
        log_pane = self._add_command_log_pane(
            title=title,
            loggers=[background_task.logger],
            background_task=background_task,
        )
        self.tasks.append(background_task)
        background_task.start_command_thread()
        return log_pane

    def _add_command_log_pane(
        self,
        title: str,
        loggers: list[logging.Logger],
        background_task: BackgroundTask,
    ) -> LogPane:
        """Create a new command log pane."""
        new_log_pane = LogPane(
            application=self.application,
            pane_title=title,
            background_task=background_task,
        )
        for logger in loggers:
            new_log_pane.add_log_handler(logger, level_name='DEBUG')

        if not background_task.process_log:
            # No custom log formatting available, disable table view.
            new_log_pane.table_view = False

        # Blank right side toolbar text
        new_log_pane._pane_subtitle = ' '  # pylint: disable=protected-access

        return new_log_pane

    def stop_all_background_commands(self) -> None:
        if not self.tasks:
            return

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=len(self.tasks)
        ) as executor:
            futures = []
            for task in self.tasks:
                task.stop_process()
                futures.append(
                    executor.submit(
                        task._wait_for_terminate_then_kill  # pylint: disable=protected-access
                    )
                )
            for future in concurrent.futures.as_completed(futures):
                future.result()
