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
"""Infrastructure used by the user interface or specific emulators."""

import io
import json
import logging
import os
import re
import socket
import subprocess
import sys
import time

from abc import ABC, abstractmethod
from importlib import import_module
from pathlib import Path
from typing import Any, Type

import psutil  # type: ignore

from pw_emu.pigweed_emulators import pigweed_emulators
from pw_env_setup.config_file import load as pw_config_load
from pw_env_setup.config_file import path as pw_config_path
from serial import Serial


_LAUNCHER_LOG = logging.getLogger('pw_qemu.core.launcher')


def _stop_process(pid: int) -> None:
    """Gracefully stops a running process."""

    try:
        proc = psutil.Process(pid)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except psutil.TimeoutExpired:
            proc.kill()
    except psutil.NoSuchProcess:
        pass


def _get_class(name: str) -> type:
    """Returns a class from a full qualified class name
    (e.g. "package.module.Class").

    """
    try:
        module_path, class_name = name.rsplit('.', 1)
        module = import_module(module_path)
        return getattr(module, class_name)
    except (ImportError, AttributeError):
        raise ImportError(name)


class Error(Exception):
    """Generic pw_emu exception."""


class ConfigError(Error):
    """Exception raised for configuration errors."""

    def __init__(self, config: Path | None, err: str) -> None:
        msg = f'{config}: {err}\n'
        try:
            if config:
                with open(config, 'r') as file:
                    msg += json.dumps(json.load(file), indent=4)
        except (OSError, json.decoder.JSONDecodeError):
            pass
        super().__init__(msg)


class AlreadyRunning(Error):
    """Exception raised if an emulator process is already running."""

    def __init__(self, wdir: Path) -> None:
        super().__init__(f'{wdir}: emulator already started')


class NotRunning(Error):
    """Exception raised if an emulator process is not running."""

    def __init__(self, wdir: Path) -> None:
        super().__init__(f'{wdir}: emulator not started')


class InvalidEmulator(Error):
    """Exception raised if an different backend is running."""

    def __init__(self, emu: str) -> None:
        super().__init__(f'invalid emulator `{emu}`')


class InvalidTarget(Error):
    """Exception raised if the target is invalid."""

    def __init__(self, config: Path, emu: str | None, target: str) -> None:
        emu_str = f'for `{emu}`' if emu else ''
        super().__init__(f'{config}: invalid target `{target}` {emu_str}')


class InvalidChannelName(Error):
    """Exception raised if a channel name is invalid."""

    def __init__(self, name: str, target: str, valid: str) -> None:
        msg = f"""
        `{name}` is not a valid device name for {target}`
        try: {valid}
        """
        super().__init__(msg)


class InvalidChannelType(Error):
    """Exception raised if a channel type is invalid."""

    def __init__(self, name: str) -> None:
        super().__init__(f'`{name}` is not a valid channel type')


class WrongEmulator(Error):
    """Exception raised if a different backend is running."""

    def __init__(self, exp: str, found: str) -> None:
        super().__init__(f'wrong emulator: expected `{exp}, found {found}`')


class RunError(Error):
    """Exception raised when a command failed to run."""

    def __init__(self, proc: str, msg: str) -> None:
        super().__init__(f'error running `{proc}`: {msg}')
        self.proc = proc
        self.msg = msg


class InvalidPropertyPath(Error):
    """Exception raised for an invalid property path."""

    def __init__(self, path: str) -> None:
        super().__init__(f'invalid property path `{path}`')


class InvalidProperty(Error):
    """Exception raised for an invalid property path."""

    def __init__(self, path: str, name: str) -> None:
        super().__init__(f'invalid property `{name}` at `{path}`')


class HandlesError(Error):
    """Exception raised if the load of an emulator handle fails."""

    def __init__(self, msg: str) -> None:
        super().__init__(f'error loading handles: {msg}')


class Handles:
    """Running emulator handles."""

    class Channel:
        def __init__(self, chan_type: str):
            self.type = chan_type

    class PtyChannel(Channel):
        def __init__(self, path: str):
            super().__init__('pty')
            self.path = path

    class TcpChannel(Channel):
        def __init__(self, host: str, port: int):
            super().__init__('tcp')
            self.host = host
            self.port = port

    class Proc:
        def __init__(self, pid: int):
            self.pid = pid

    @staticmethod
    def _ser_obj(obj) -> Any:
        if isinstance(obj, dict):
            data = {}
            for key, val in obj.items():
                data[key] = Handles._ser_obj(val)
            return data
        if hasattr(obj, "__iter__") and not isinstance(obj, str):
            return [Handles._ser_obj(item) for item in obj]
        if hasattr(obj, "__dict__"):
            return Handles._ser_obj(obj.__dict__)
        return obj

    def _serialize(self):
        return Handles._ser_obj(self)

    def save(self, wdir: Path) -> None:
        """Saves handles to the given working directory."""

        with open(os.path.join(wdir, 'handles.json'), 'w') as file:
            json.dump(self._serialize(), file)

    @staticmethod
    def load(wdir: Path):
        try:
            with open(os.path.join(wdir, 'handles.json'), 'r') as file:
                data = json.load(file)
        except (KeyError, OSError, json.decoder.JSONDecodeError):
            raise NotRunning(wdir)

        handles = Handles(data['emu'], data['config'])
        handles.set_target(data['target'])
        gdb_cmd = data.get('gdb_cmd')
        if gdb_cmd:
            handles.set_gdb_cmd(gdb_cmd)
        for name, chan in data['channels'].items():
            chan_type = chan['type']
            if chan_type == 'tcp':
                handles.add_channel_tcp(name, chan['host'], chan['port'])
            elif chan_type == 'pty':
                handles.add_channel_pty(name, chan['path'])
            else:
                raise InvalidChannelType(chan_type)
        for name, proc in data['procs'].items():
            handles.add_proc(name, proc['pid'])
        return handles

    def __init__(self, emu: str, config: str) -> None:
        self.emu = emu
        self.config = config
        self.gdb_cmd: list[str] = []
        self.target = ''
        self.channels: dict[str, Handles.Channel] = {}
        self.procs: dict[str, Handles.Proc] = {}

    def add_channel_tcp(self, name: str, host: str, port: int) -> None:
        """Adds a TCP channel."""

        self.channels[name] = self.TcpChannel(host, port)

    def add_channel_pty(self, name: str, path: str) -> None:
        """Adds a pty channel."""

        self.channels[name] = self.PtyChannel(path)

    def add_proc(self, name: str, pid: int) -> None:
        """Adds a process ID."""

        self.procs[name] = self.Proc(pid)

    def set_target(self, target: str) -> None:
        """Sets the target."""

        self.target = target

    def set_gdb_cmd(self, cmd: list[str]) -> None:
        """Sets the ``gdb`` command."""

        self.gdb_cmd = cmd.copy()


def _stop_processes(handles: Handles, wdir: Path) -> None:
    """Stops all processes for a (partially) running emulator instance.

    Remove pid files as well.
    """

    for _, proc in handles.procs.items():
        _stop_process(proc.pid)
        path = os.path.join(wdir, f'{proc}.pid')
        if os.path.exists(path):
            os.unlink(path)


class Config:
    """Get and validate options from the configuration file."""

    def __init__(
        self,
        config_path: Path | None = None,
        target: str | None = None,
        emu: str | None = None,
    ) -> None:
        """Loads the emulator configuration.

        If no configuration file path is given, the root project
        configuration is used.

        This method set ups the generic configuration (e.g. ``gdb``).

        It loads emulator target files and gathers them under the ``targets``
        key for each emulator backend. The ``targets`` settings in the
        configuration file takes precedence over the loaded target files.

        """
        try:
            if config_path:
                with open(config_path, 'r') as file:
                    config = json.load(file)['pw']['pw_emu']
            else:
                config_path = pw_config_path()
                config = pw_config_load()['pw']['pw_emu']
        except KeyError:
            raise ConfigError(config_path, 'missing `pw_emu` configuration')

        if not config_path:
            raise ConfigError(None, 'unable to deterine config path')

        if config.get('target_files'):
            tmp = {}
            for path in config['target_files']:
                if not os.path.isabs(path):
                    path = os.path.join(os.path.dirname(config_path), path)
                with open(path, 'r') as file:
                    tmp.update(json.load(file).get('targets'))
            if config.get('targets'):
                tmp.update(config['targets'])
            config['targets'] = tmp

        self.path = config_path
        self._config = {'emulators': pigweed_emulators}
        self._config.update(config)
        self._emu = emu
        self._target = target

    def set_target(self, target: str) -> None:
        """Sets the current target.

        The current target is used by the
        :py:meth:`pw_emu.core.Config.get_target` method.

        """

        self._target = target
        try:
            self.get(['targets', target], optional=False, entry_type=dict)
        except ConfigError:
            raise InvalidTarget(self.path, self._emu, self._target)

    def get_targets(self) -> list[str]:
        return list(self.get(['targets'], entry_type=dict).keys())

    def _subst(self, string: str) -> str:
        """Substitutes $pw_<subst_type>{arg} statements."""

        match = re.search(r'\$pw_([^{]+){([^}]+)}', string)
        if not match:
            return string

        subst_type = match.group(1)
        arg = match.group(2)

        if subst_type == 'env':
            value = os.environ.get(arg)
            if value is None:
                msg = f'Environment variable `{arg}` not set'
                raise ConfigError(self.path, msg)
            return string.replace(f'$pw_{subst_type}{{{arg}}}', value)

        raise ConfigError(self.path, f'Invalid substitution type: {subst_type}')

    def _subst_list(self, items: list[Any]) -> list[Any]:
        new_list = []
        for item in items:
            if isinstance(item, str):
                new_list.append(self._subst(item))
            else:
                new_list.append(item)
        return new_list

    def get(
        self,
        keys: list[str],
        optional: bool = True,
        entry_type: Type | None = None,
    ) -> Any:
        """Gets a config entry.

        ``keys`` identifies the config entry, e.g.
        ``['targets', 'test-target']`` looks in the config dictionary for
        ``['targets']['test-target']``.

        If the option is not found and optional is ``True`` it returns ``None``
        if ``entry_type`` is ``None`` or a new (empty) object of type
        ``entry_type``.

        If the option is not found and ``optional`` is ``False`` it raises
        ``ConfigError``.

        If ``entry_type`` is not ``None`` it checks the option to be of
        that type. If it is not it will raise ``ConfigError``.

        """

        keys_str = ': '.join(keys)
        entry: Any = self._config

        for key in keys:
            if not isinstance(entry, dict):
                if optional:
                    if entry_type:
                        return entry_type()
                    return None
                raise ConfigError(self.path, f'{keys_str}: not found')
            entry = entry.get(key)

        if entry is None:
            if optional:
                if entry_type:
                    return entry_type()
                return None
            raise ConfigError(self.path, f'{keys_str}: not found')

        if entry_type and not isinstance(entry, entry_type):
            msg = f'{keys_str}: expected entry of type `{entry_type}`'
            raise ConfigError(self.path, msg)

        if isinstance(entry, str):
            entry = self._subst(entry)
        elif isinstance(entry, list):
            entry = self._subst_list(entry)

        return entry

    def get_target(
        self,
        keys: list[str],
        optional: bool = True,
        entry_type: Type | None = None,
    ) -> Any:
        """Gets a config option starting at ``['targets'][target]``."""

        if not self._target:
            raise Error('target not set')
        return self.get(['targets', self._target] + keys, optional, entry_type)

    def get_emu(
        self,
        keys: list[str],
        optional: bool = True,
        entry_type: Type | None = None,
    ) -> Any:
        """Gets a config option starting at ``[emu]``."""

        if not self._emu:
            raise Error('emu not set')
        return self.get([self._emu] + keys, optional, entry_type)

    def get_target_emu(
        self,
        keys: list[str],
        optional: bool = True,
        entry_type: Type | None = None,
    ) -> Any:
        """Gets a config option starting at ``['targets'][target][emu]``."""

        if not self._emu or not self._target:
            raise Error('emu or target not set')
        return self.get(
            ['targets', self._target, self._emu] + keys, optional, entry_type
        )


class Connector(ABC):
    """Interface between a running emulator and the user-visible APIs."""

    def __init__(self, wdir: Path) -> None:
        self._wdir = wdir
        self._handles = Handles.load(wdir)
        self._channels = self._handles.channels
        self._target = self._handles.target

    @staticmethod
    def get(wdir: Path) -> Any:
        """Returns a connector instance for a given emulator type."""
        handles = Handles.load(wdir)
        config = Config(handles.config)
        emu = handles.emu
        try:
            name = config.get(['emulators', emu, 'connector'])
            cls = _get_class(name)
        except (ConfigError, ImportError):
            raise InvalidEmulator(emu)
        return cls(wdir)

    def get_emu(self) -> str:
        """Returns the emulator type."""

        return self._handles.emu

    def get_gdb_cmd(self) -> list[str]:
        """Returns the configured ``gdb`` command."""
        return self._handles.gdb_cmd

    def get_config_path(self) -> Path:
        """Returns the configuration path."""

        return self._handles.config

    def get_procs(self) -> dict[str, Handles.Proc]:
        """Returns the running processes indexed by the process name."""

        return self._handles.procs

    def get_channel_type(self, name: str) -> str:
        """Returns the channel type."""

        try:
            return self._channels[name].type
        except KeyError:
            channels = ' '.join(self._channels.keys())
            raise InvalidChannelName(name, self._target, channels)

    def get_channel_path(self, name: str) -> str:
        """Returns the channel path. Raises ``InvalidChannelType`` if this
        is not a ``pty`` channel.

        """

        try:
            if self._channels[name].type != 'pty':
                raise InvalidChannelType(self._channels[name].type)
            return self._channels[name].path
        except KeyError:
            raise InvalidChannelName(name, self._target, self._channels.keys())

    def get_channel_addr(self, name: str) -> tuple:
        """Returns a pair of ``(host, port)`` for the channel. Raises
        ``InvalidChannelType`` if this is not a ``tcp`` channel.

        """

        try:
            if self._channels[name].type != 'tcp':
                raise InvalidChannelType(self._channels[name].type)
            return (self._channels[name].host, self._channels[name].port)
        except KeyError:
            raise InvalidChannelName(name, self._target, self._channels.keys())

    def get_channel_stream(
        self,
        name: str,
        timeout: float | None = None,
    ) -> io.RawIOBase:
        """Returns a file object for a given host-exposed device.

        If ``timeout`` is ``None`` then reads and writes are blocking. If
        ``timeout`` is ``0`` the stream is operating in non-blocking
        mode. Otherwise reads and writes will timeout after the given
        value.

        """

        chan_type = self.get_channel_type(name)
        if chan_type == 'tcp':
            host, port = self.get_channel_addr(name)
            if ':' in host:
                sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            else:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((host, port))
            sock.settimeout(timeout)
            ret = sock.makefile('rwb', buffering=0)
            sock.close()
            return ret
        if chan_type == 'pty':
            ser = Serial(self.get_channel_path(name))
            ser.timeout = timeout
            return ser
        raise InvalidChannelType(chan_type)

    def get_channels(self) -> list[str]:
        return self._handles.channels.keys()

    def get_logs(self) -> str:
        """Returns the emulator logs."""

        log_path = self._wdir / f'{self._handles.emu}.log'
        return log_path.read_text()

    def stop(self) -> None:
        """Stops the emulator."""

        _stop_processes(self._handles, self._wdir)

        try:
            os.unlink(os.path.join(self._wdir, 'handles.json'))
        except OSError:
            pass

    def proc_running(self, proc: str) -> bool:
        try:
            return psutil.pid_exists(self._handles.procs[proc].pid)
        except (NotRunning, KeyError):
            return False

    def running(self) -> bool:
        """Checks if the main emulator process is already running."""

        try:
            return psutil.pid_exists(self._handles.procs[self._handles.emu].pid)
        except (NotRunning, KeyError):
            return False

    @abstractmethod
    def reset(self) -> None:
        """Performs a software reset."""

    @abstractmethod
    def cont(self) -> None:
        """Resumes the emulator's execution."""

    @abstractmethod
    def list_properties(self, path: str) -> list[Any]:
        """Returns the property list for an emulator object."""

    @abstractmethod
    def set_property(self, path: str, prop: str, value: str) -> None:
        """Sets the value of an emulator's object property."""

    @abstractmethod
    def get_property(self, path: str, prop: str) -> Any:
        """Returns the value of an emulator's object property."""


class Launcher(ABC):
    """Starts an emulator based on the target and configuration file."""

    def __init__(
        self,
        emu: str,
        config_path: Path | None = None,
    ) -> None:
        """Initializes a ``Launcher`` instance."""

        self._wdir: Path | None = None
        """Working directory"""

        self._emu = emu
        """Emulator type (e.g. "qemu", "renode")."""

        self._target: str | None = None
        """Target, initialized to None and set with _prep_start."""

        self._config = Config(config_path, emu=emu)
        """Global, emulator and  target configuration."""

        self._handles = Handles(self._emu, str(self._config.path))
        """Handles for processes, channels, etc."""

        gdb_cmd = self._config.get(['gdb'], entry_type=list)
        if gdb_cmd:
            self._handles.set_gdb_cmd(gdb_cmd)

    @staticmethod
    def get(emu: str, config_path: Path | None = None) -> Any:
        """Returns a launcher for a given emulator type."""
        config = Config(config_path)
        try:
            name = config.get(['emulators', emu, 'launcher'])
            cls = _get_class(name)
        except (ConfigError, ImportError):
            raise InvalidEmulator(str(emu))
        return cls(config_path)

    @abstractmethod
    def _pre_start(
        self,
        target: str,
        file: Path | None = None,
        pause: bool = False,
        debug: bool = False,
        args: str | None = None,
    ) -> list[str]:
        """Pre-start work, returns command to start the emulator.

        The target and emulator configuration can be accessed through
        :py:attr:`pw_emu.core.Launcher._config` with
        :py:meth:`pw_emu.core.Config.get`,
        :py:meth:`pw_emu.core.Config.get_target`,
        :py:meth:`pw_emu.core.Config.get_emu`,
        :py:meth:`pw_emu.core.Config.get_target_emu`.
        """

    @abstractmethod
    def _post_start(self) -> None:
        """Post-start work, finalize emulator handles.

        Perform any post-start emulator initialization and finalize the emulator
        handles information.

        Typically an internal monitor channel is used to inquire information
        about the configured channels (e.g. TCP ports, pty paths) and
        :py:attr:`pw_emu.core.Launcher._handles` is updated via
        :py:meth:`pw_emu.core.Handles.add_channel_tcp`,
        :py:meth:`pw_emu.core.Handles.add_channel_pty`, etc.

        """

    @abstractmethod
    def _get_connector(self, wdir: Path) -> Connector:
        """Gets a connector for this emulator type."""

    def _path(self, name: Path | str) -> Path:
        """Returns the full path for a given emulator file."""
        if self._wdir is None:
            raise Error('internal error')
        return Path(os.path.join(self._wdir, name))

    def _subst_channel(self, subst_type: str, arg: str, string: str) -> str:
        """Substitutes $pw_emu_channel_{func}{arg} statements."""

        try:
            chan = self._handles.channels[arg]
        except KeyError:
            return string

        if subst_type == 'channel_port':
            if not isinstance(chan, Handles.TcpChannel):
                return string
            return str(chan.port)

        if subst_type == 'channel_host':
            if not isinstance(chan, Handles.TcpChannel):
                return string
            return chan.host

        if subst_type == 'channel_path':
            if not isinstance(chan, Handles.PtyChannel):
                return string
            return chan.path

        return string

    def _subst(self, string: str) -> str:
        """Substitutes $pw_emu_<subst_type>{arg} statements."""

        match = re.search(r'\$pw_emu_([^{]+){([^}]+)}', string)
        if not match:
            return string

        subst_type = match.group(1)
        arg = match.group(2)

        if subst_type == 'wdir':
            if self._wdir:
                return os.path.join(self._wdir, arg)
            return string

        if 'channel_' in subst_type:
            return self._subst_channel(subst_type, arg, string)

        return string

    # pylint: disable=protected-access
    # use os._exit after fork instead of os.exit
    def _daemonize(
        self,
        name: str,
        cmd: list[str],
    ) -> None:
        """Daemonize process for UNIX hosts."""

        if sys.platform == 'win32':
            raise Error('_daemonize not supported on win32')

        # pylint: disable=no-member
        # avoid pylint false positive on win32
        pid = os.fork()  # pyrefly: ignore[missing-attribute]
        if pid < 0:
            raise RunError(name, f'fork failed: {pid}')
        if pid > 0:
            return

        path: Path = Path('/dev/null')
        fd = os.open(path, os.O_RDONLY)
        os.dup2(fd, sys.stdin.fileno())
        os.close(fd)

        path = self._path(f'{name}.log')
        fd = os.open(path, os.O_WRONLY | os.O_TRUNC | os.O_CREAT)
        os.dup2(fd, sys.stdout.fileno())
        os.dup2(fd, sys.stderr.fileno())
        os.close(fd)

        os.setsid()  # pyrefly: ignore[missing-attribute]

        if os.fork() > 0:  # pyrefly: ignore[missing-attribute]
            os._exit(0)

        try:
            # Make the pid file create and pid write operations atomic to avoid
            # races with readers.
            with open(self._path(f'{name}.pid.tmp'), 'w') as file:
                file.write(f'{os.getpid()}')
            os.rename(self._path(f'{name}.pid.tmp'), self._path(f'{name}.pid'))
            os.execvp(cmd[0], cmd)
        finally:
            os._exit(1)

    def _start_proc(
        self,
        name: str,
        cmd: list[str],
        foreground: bool = False,
    ) -> subprocess.Popen | None:
        """Run the main emulator process.

        The process pid is stored and can later be accessed by its name to
        terminate it when the emulator is stopped.

        If foreground is True the process run in the foreground and a
        subprocess.Popen object is returned. Otherwise the process is started in
        the background and None is returned.

        When running in the background stdin is redirected to the NULL device
        and stdout and stderr are redirected to a file named <name>.log which is
        stored in the emulator's instance working directory.

        """
        for idx, item in enumerate(cmd):
            cmd[idx] = self._subst(item)

        pid_file_path = self._path(f'{name}.pid')
        if os.path.exists(pid_file_path):
            os.unlink(pid_file_path)

        if foreground:
            proc = subprocess.Popen(cmd)
            self._handles.add_proc(name, proc.pid)
            with open(pid_file_path, 'w') as file:
                file.write(f'{proc.pid}')
            return proc

        if sys.platform == 'win32':
            file = open(self._path(f'{name}.log'), 'w')
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=file,
                stderr=file,
                creationflags=subprocess.DETACHED_PROCESS,
            )
            file.close()
            with open(pid_file_path, 'w') as file:
                file.write(f'{proc.pid}')
            self._handles.add_proc(name, proc.pid)
            # avoids resource warnings due to not calling wait which
            # we don't want to do since we've started the process in
            # the background
            proc.returncode = 0
        else:
            self._daemonize(name, cmd)

        # wait for the pid file to avoid double start race conditions
        timeout = time.monotonic() + 30
        while not os.path.exists(self._path(f'{name}.pid')):
            time.sleep(0.1)
            if time.monotonic() > timeout:
                break
        if not os.path.exists(self._path(f'{name}.pid')):
            raise RunError(name, 'pid file timeout')
        try:
            with open(pid_file_path, 'r') as file:
                pid = int(file.readline())
                self._handles.add_proc(name, pid)
        except (OSError, ValueError) as err:
            raise RunError(name, str(err))

        return None

    def _stop_procs(self):
        """Stop all registered processes."""

        for name, proc in self._handles.procs.items():
            _stop_process(proc.pid)
            if os.path.exists(self._path(f'{name}.pid')):
                os.unlink(self._path(f'{name}.pid'))

    def _start_procs(self, procs_list: str) -> None:
        """Start additional processes besides the main emulator one."""

        procs = self._config.get_target([procs_list], entry_type=dict)
        for name, cmd in procs.items():
            self._start_proc(name, cmd)

    def start(
        self,
        wdir: Path,
        target: str,
        file: Path | None = None,
        pause: bool = False,
        debug: bool = False,
        foreground: bool = False,
        args: str | None = None,
    ) -> Connector:
        """Starts the emulator for the given target.

        If ``file`` is set the emulator loads that file before starting.

        If ``pause`` is ``True`` the emulator gets paused.

        If ``debug`` is ``True`` the emulator runs in the foreground with
        debug output enabled. This is useful for seeing errors, traces, etc.

        If ``foreground`` is ``True`` the emulator is run in the foreground
        otherwise it is started in daemon mode. This is useful when there is
        another process controlling the emulator's life cycle, e.g. cuttlefish.

        ``args`` are passed directly to the emulator.

        """

        try:
            handles = Handles.load(wdir)
            if psutil.pid_exists(handles.procs[handles.emu].pid):
                raise AlreadyRunning(wdir)
        except NotRunning:
            pass

        self._wdir = wdir
        self._target = target
        self._config.set_target(target)
        self._handles.set_target(target)
        gdb_cmd = self._config.get_target(['gdb'], entry_type=list)
        if gdb_cmd:
            self._handles.set_gdb_cmd(gdb_cmd)
        os.makedirs(wdir, mode=0o700, exist_ok=True)

        cmd = self._pre_start(
            target=target, file=file, pause=pause, debug=debug, args=args
        )

        if debug:
            foreground = True
            _LAUNCHER_LOG.setLevel(logging.DEBUG)

        _LAUNCHER_LOG.debug('starting emulator with command: %s', ' '.join(cmd))

        try:
            self._start_procs('pre-start-cmds')
            proc = self._start_proc(self._emu, cmd, foreground)
            self._start_procs('post-start-cmds')
        except RunError as err:
            self._stop_procs()
            raise err

        try:
            self._post_start()
        except RunError as err:
            self._handles.save(wdir)
            connector = self._get_connector(self._wdir)
            if not connector.running():
                msg = err.msg + '; dumping logs:\n' + connector.get_logs()
                raise RunError(err.proc, msg)
            raise err
        self._handles.save(wdir)

        if proc:
            proc.wait()
            self._stop_procs()

        return self._get_connector(self._wdir)
