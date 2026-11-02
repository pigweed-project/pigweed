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
"""Pigweed qemu frontend."""


import io
import json
import logging
import os
import re
import socket
import sys

from pathlib import Path
from typing import Any

from pw_emu.core import (
    ConfigError,
    Connector,
    Launcher,
    Error,
    InvalidChannelType,
    RunError,
    WrongEmulator,
)

_QMP_LOG = logging.getLogger('pw_qemu.qemu.qmp')


class QmpError(Error):
    """Exception for QMP errors."""

    def __init__(self, err: str):
        super().__init__(err)


class QmpClient:
    """Send qmp requests the server."""

    def __init__(self, stream: io.RawIOBase):
        self._stream = stream

        # Perform the QMP "capabilities negotiation" handshake.
        # https://wiki.qemu.org/Documentation/QMP#Capabilities_Negotiation
        #
        # When the QMP connection is established, QEMU first sends a greeting
        # message with its version and capabilities. Then the client sends
        # 'qmp_capabilities' to exit capabilities negotiation mode. The result
        # is an empty 'return'.
        #
        # self.request() will consume both the initial greeting and the
        # subsequent 'return' response.
        self.request('qmp_capabilities')

    def request(self, cmd: str, args: dict[str, Any] | None = None) -> Any:
        """Issue a command using the qmp interface.

        Returns a map with the response or None if there is no
        response for this command.

        """

        req: dict[str, Any] = {'execute': cmd}
        if args:
            req['arguments'] = args
        _QMP_LOG.debug(' -> {json.dumps(cmd)}')
        self._stream.write(json.dumps(req).encode('utf-8'))
        while True:
            line = self._stream.readline()
            _QMP_LOG.debug(' <- {line}')
            resp = json.loads(line)
            if 'error' in resp.keys():
                raise QmpError(resp['error']['desc'])
            if 'return' in resp.keys():
                return resp['return']


class QemuLauncher(Launcher):
    """Start a new qemu process for a given target and config file."""

    def __init__(self, config_path: Path | None = None):
        super().__init__('qemu', config_path)
        self._start_cmd: list[str] = []
        self._chardevs_id_to_name = {
            'compat_monitor0': 'qmp',
            'compat_monitor1': 'monitor',
            'gdb': 'gdb',
        }
        self._chardevs: dict[str, Any] = {}
        self._qmp_init_sock: socket.socket | None = None

    def _set_qemu_channel_tcp(self, name: str, filename: str) -> None:
        """Parse a TCP chardev and return (host, port) tuple.

        Format for the tcp chardev backend:

        [disconnected|isconnected:]tcp:<host>:<port>[,<options>][ <->
        <host>:<port>]

        """

        host_port: Any = filename.split(',')[0]
        if host_port.split(':')[0] != 'tcp':
            host_port = host_port.split(':')[2:]
        else:
            host_port = host_port.split(':')[1:]
        # IPV6 hosts have :
        host = ':'.join(host_port[0:-1])
        port = host_port[-1]
        self._handles.add_channel_tcp(name, host, int(port))

    def _set_qemu_channel_pty(self, name: str, filename: str) -> None:
        """Parse a PTY chardev and return the path.

        Format for the pty chardev backend: pty:<path>
        """

        path = filename.split(':')[1]

        self._handles.add_channel_pty(name, path)

        if os.path.lexists(self._path(name)):
            os.unlink(self._path(name))
        os.symlink(path, self._path(name))

    def _set_qemu_channel(self, name: str, filename: str) -> None:
        """Setups a chardev channel type."""

        if filename.startswith('pty'):
            self._set_qemu_channel_pty(name, filename)
        elif 'tcp' in filename:
            self._set_qemu_channel_tcp(name, filename)

    def _get_channels_config(self, chan: str, opt: str) -> Any:
        val = self._config.get_emu(['channels', chan, opt])
        if val is not None:
            return val
        return self._config.get_emu(['channels', opt])

    def _configure_default_channels(self) -> None:
        """Configure the default channels."""

        # keep qmp first so that it gets the compat_monitor0 label
        for chan in ['qmp', 'monitor', 'gdb']:
            chan_type = self._get_channels_config(chan, 'type')
            if not chan_type:
                chan_type = 'tcp'
            if chan_type == 'pty':
                if sys.platform == 'win32':
                    raise InvalidChannelType(chan_type)
                backend = 'pty'
            elif chan_type == 'tcp':
                backend = 'tcp:localhost:0,ipv4=on,server=on,wait=off'
            else:
                raise InvalidChannelType(chan_type)
            self._start_cmd.extend([f'-{chan}', backend])

    def _get_chardev_config(self, name: str, opt: str) -> Any:
        val = self._config.get_target_emu(['channels', 'chardevs', name, opt])
        if not val:
            val = self._get_channels_config(name, opt)
        return val

    def _configure_serial_channels(self, serials: dict) -> None:
        """Create "standard" serial devices.

        We can't control the serial allocation number for "standard"
        -serial devices so fill the slots for the not needed serials
        with null chardevs e.g. for serial3, serial1 generate the
        following arguments, in this order:

         -serial null -serial {backend} -serial null - serial {backend}

        """

        min_ser = sys.maxsize
        max_ser = -1
        for serial in serials.keys():
            num = int(serial.split('serial')[1])
            min_ser = min(min_ser, num)
            max_ser = max(max_ser, num)
        for i in range(min_ser, max_ser + 1):
            if serials.get(f'serial{i}'):
                name = serials[f'serial{i}']
                chan_type = self._get_chardev_config(name, 'type')
                if not chan_type:
                    chan_type = 'tcp'
                if chan_type == 'pty':
                    backend = 'pty'
                elif chan_type == 'tcp':
                    backend = 'tcp:localhost:0,ipv4=on,server=on,wait=off'
                else:
                    raise InvalidChannelType(chan_type)
                self._start_cmd.extend(['-serial', backend])
            else:
                self._start_cmd.extend(['-serial', 'null'])

    def _configure_chardev_channels(self) -> None:
        """Configure chardevs."""

        self._chardevs = self._config.get_target_emu(
            ['channels', 'chardevs'], True, dict
        )

        serials = {}
        for name, config in self._chardevs.items():
            chardev_id = config['id']
            self._chardevs_id_to_name[chardev_id] = name

            chardev_type = self._get_chardev_config(name, 'type')
            if chardev_type is None:
                chardev_type = 'tcp'

            if chardev_type == 'pty':
                backend = 'pty'
            elif chardev_type == 'tcp':
                backend = 'socket,host=localhost,port=0,server=on,wait=off'
            else:
                raise InvalidChannelType(chardev_type)

            # serials are configured differently
            if re.search(r'serial[0-9]*', chardev_id):
                serials[chardev_id] = name
            else:
                self._start_cmd.extend(
                    ['-chardev', f'{backend},id={chardev_id}']
                )

        self._configure_serial_channels(serials)

    def _pre_start(
        self,
        target: str,
        file: Path | None = None,
        pause: bool = False,
        debug: bool = False,
        args: str | None = None,
    ) -> list[str]:
        qemu = self._config.get_target_emu(['executable'])
        if not qemu:
            qemu = self._config.get_emu(['executable'], optional=False)
        machine = self._config.get_target_emu(['machine'], optional=False)

        self._start_cmd = [f'{qemu}', '-nographic', '-nodefaults']
        self._start_cmd.extend(['-display', 'none'])
        self._start_cmd.extend(['-machine', f'{machine}'])

        try:
            self._configure_default_channels()
            self._configure_chardev_channels()
        except KeyError as err:
            raise ConfigError(self._config.path, str(err))

        if pause:
            self._start_cmd.append('-S')
        if debug:
            self._start_cmd.extend(['-d', 'guest_errors'])

        if file:
            self._start_cmd.extend(['-kernel', str(file)])

        self._start_cmd.extend(self._config.get_emu(['args'], entry_type=list))
        self._start_cmd.extend(
            self._config.get_target_emu(['args'], entry_type=list)
        )
        if args:
            self._start_cmd.extend(args.split(' '))

        # initial/bootstrap qmp connection
        self._qmp_init_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._qmp_init_sock.bind(('localhost', 0))
        port = self._qmp_init_sock.getsockname()[1]
        self._qmp_init_sock.listen()
        self._qmp_init_sock.settimeout(30)
        self._start_cmd.extend(['-qmp', f'tcp:localhost:{port}'])

        return self._start_cmd

    def _post_start(self) -> None:
        assert self._qmp_init_sock is not None
        try:
            conn, _ = self._qmp_init_sock.accept()
        except (KeyboardInterrupt, TimeoutError):
            raise RunError('qemu', 'qmp connection failed')
        self._qmp_init_sock.close()
        try:
            qmp = QmpClient(conn.makefile('rwb', buffering=0))
        except json.decoder.JSONDecodeError:
            raise RunError('qemu', 'qmp handshake failed')
        conn.close()

        resp = qmp.request('query-chardev')
        for chardev in resp:
            label = chardev['label']
            name = self._chardevs_id_to_name.get(label)
            if name:
                self._set_qemu_channel(name, chardev['filename'])

    def _get_connector(self, wdir: Path) -> Connector:
        return QemuConnector(wdir)


class QemuConnector(Connector):
    """qemu implementation for the emulator specific connector methods."""

    def __init__(self, wdir: Path) -> None:
        super().__init__(wdir)
        if self.get_emu() != 'qemu':
            raise WrongEmulator('qemu', self.get_emu())
        self._qmp: QmpClient | None = None

    def _q(self) -> QmpClient:
        if not self._qmp:
            self._qmp = QmpClient(self.get_channel_stream('qmp'))
        return self._qmp

    def reset(self) -> None:
        self._q().request('system_reset')

    def cont(self) -> None:
        self._q().request('cont')

    def set_property(self, path: str, prop: str, value: Any) -> None:
        args = {
            'path': '{}'.format(path),
            'property': prop,
            'value': value,
        }
        self._q().request('qom-set', args)

    def get_property(self, path: str, prop: str) -> Any:
        args = {
            'path': '{}'.format(path),
            'property': prop,
        }
        return self._q().request('qom-get', args)

    def list_properties(self, path: str) -> list[Any]:
        args = {
            'path': '{}'.format(path),
        }
        return self._q().request('qom-list', args)
