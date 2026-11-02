# Copyright 2019 The Pigweed Authors
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
"""Module for running subprocesses from pw and capturing their output."""

from __future__ import annotations

import asyncio
import logging
import os
import shlex
import tempfile
from typing import IO

import pw_cli.color
import pw_cli.log

import psutil  # type: ignore


_COLOR = pw_cli.color.colors()
_LOG = logging.getLogger(__name__)

# Environment variable passed down to subprocesses to indicate that they are
# running as a subprocess. Can be imported by other code.
PW_SUBPROCESS_ENV = 'PW_SUBPROCESS'


class CompletedProcess:
    """Information about a process executed in run_async.

    Attributes:
        pid: The process identifier.
        returncode: The return code of the process.
    """

    def __init__(
        self,
        process: asyncio.subprocess.Process,
        output: bytes | IO[bytes],
    ):
        assert process.returncode is not None
        self.returncode: int = process.returncode
        self.pid = process.pid
        self._output = output

    @property
    def output(self) -> bytes:
        # If the output is a file, read it, then close it.
        if not isinstance(self._output, bytes):
            with self._output as file:
                file.flush()
                file.seek(0)
                self._output = file.read()

        return self._output


async def _run_and_log(program: str, args: tuple[str, ...], env: dict):
    process = await asyncio.create_subprocess_exec(
        program,
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        env=env,
    )

    output = bytearray()

    if process.stdout:
        while True:
            line = await process.stdout.readline()
            if not line:
                break

            output += line
            _LOG.log(
                pw_cli.log.LOGLEVEL_STDOUT,
                '[%s] %s',
                _COLOR.bold_white(process.pid),
                line.decode(errors='replace').rstrip(),
            )

    return process, bytes(output)


async def _kill_process_and_children(
    process: asyncio.subprocess.Process,
) -> None:
    """Kills child processes of a process with PID `pid`."""
    # Look up child processes before sending the kill signal to the parent,
    # as otherwise the child lookup would fail.
    pid = process.pid
    try:
        process_util = psutil.Process(pid=pid)
        kill_list = list(process_util.children(recursive=True))
    except psutil.NoSuchProcess:
        # Creating the kill list raced with parent process completion.
        #
        # Don't bother cleaning up child processes of parent processes that
        # exited on their own.
        kill_list = []

    for proc in kill_list:
        try:
            proc.kill()
        except psutil.NoSuchProcess:
            pass

    def wait_for_all() -> None:
        for proc in kill_list:
            try:
                proc.wait()
            except psutil.NoSuchProcess:
                pass

    # Wait for process completion on the executor to avoid blocking the
    # event loop.
    loop = asyncio.get_event_loop()
    wait_for_children = loop.run_in_executor(None, wait_for_all)

    # Send a kill signal to the main process before waiting for the children
    # to complete.
    try:
        process.kill()
        await process.wait()
    except ProcessLookupError:
        _LOG.debug(
            'Process completed before it could be killed. '
            'This may have been caused by the killing one of its '
            'child subprocesses.',
        )

    await wait_for_children


async def run_async(
    program: str,
    *args: str,
    env: dict[str, str] | None = None,
    log_output: bool = False,
    timeout: float | None = None,
) -> CompletedProcess:
    """Runs a command, capturing and optionally logging its output.

    Args:
      program: The program to run in a new process.
      args: The arguments to pass to the program.
      env: An optional mapping of environment variables within which to run
        the process.
      log_output: Whether to log stdout and stderr of the process to this
        process's stdout (prefixed with the PID of the subprocess from which
        the output originated). If unspecified, the child process's stdout
        and stderr will be captured, and both will be stored in the returned
        `CompletedProcess`'s  output`.
      timeout: An optional floating point number of seconds to allow the
        subprocess to run before killing it and its children. If unspecified,
        the subprocess will be allowed to continue exiting until it completes.

    Returns a CompletedProcess with details from the process.
    """

    _LOG.debug(
        'Running `%s`', ' '.join(shlex.quote(arg) for arg in [program, *args])
    )

    hydrated_env = os.environ.copy()
    if env is not None:
        for key, value in env.items():
            hydrated_env[key] = value
    hydrated_env[PW_SUBPROCESS_ENV] = '1'
    output: bytes | IO[bytes]

    if log_output:
        process, output = await _run_and_log(program, args, hydrated_env)
    else:
        output = tempfile.TemporaryFile()
        process = await asyncio.create_subprocess_exec(
            program,
            *args,
            stdout=output,
            stderr=asyncio.subprocess.STDOUT,
            env=hydrated_env,
        )

    try:
        await asyncio.wait_for(process.wait(), timeout)
    except TimeoutError:
        _LOG.error('%s timed out after %d seconds', program, timeout)
        await _kill_process_and_children(process)

    if process.returncode:
        _LOG.error('%s exited with status %d', program, process.returncode)
    else:
        # process.returncode is 0
        _LOG.debug('%s exited successfully', program)

    return CompletedProcess(process, output)


def run(program: str, *args: str, **kwargs) -> CompletedProcess:
    """Synchronous wrapper for run_async."""
    return asyncio.run(run_async(program, *args, **kwargs))
