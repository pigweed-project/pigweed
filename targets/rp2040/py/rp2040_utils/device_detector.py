#!/usr/bin/env python3
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
"""Detects attached Raspberry Pi Pico boards."""

from dataclasses import asdict, dataclass
import logging
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from typing import Iterable, Optional, Sequence

from ctypes.util import find_library as ctypes_find_library
import serial.tools.list_ports  # type: ignore[import-untyped]
import usb  # type: ignore
from usb.backend import libusb1  # type: ignore

import pw_cli.log
from pw_cli.env import pigweed_environment

_LOG = logging.getLogger('pi_pico_detector')

# Vendor/device ID to search for in USB devices.
_RASPBERRY_PI_VENDOR_ID = 0x2E8A

# RaspberryPi Debug probe: https://github.com/raspberrypi/debugprobe
_DEBUG_PROBE_DEVICE_ID = 0x000C

_RP2040_USB_SERIAL_DEVICE_ID = 0x000A
_RP2040_BOOTLOADER_DEVICE_ID = 0x0003
_PICO_USB_SERIAL_DEVICE_ID = 0x0009
_PICO_BOOTLOADER_DEVICE_ID = 0x000F

_PICO_USB_SERIAL_DEVICE_IDS = (
    _RP2040_USB_SERIAL_DEVICE_ID,
    _PICO_USB_SERIAL_DEVICE_ID,
)

_PICO_BOOTLOADER_DEVICE_IDS = (
    _RP2040_BOOTLOADER_DEVICE_ID,
    _PICO_BOOTLOADER_DEVICE_ID,
)

_PICO_DEVICE_IDS = (
    *_PICO_USB_SERIAL_DEVICE_IDS,
    *_PICO_BOOTLOADER_DEVICE_IDS,
)

_ALL_DEVICE_IDS = (
    _DEBUG_PROBE_DEVICE_ID,
    *_PICO_USB_SERIAL_DEVICE_IDS,
    *_PICO_BOOTLOADER_DEVICE_IDS,
)

_LIBUSB_CIPD_INSTALL_ENV_VAR = 'PW_PIGWEED_CIPD_INSTALL_DIR'
_LIBUSB_CIPD_SUBDIR = 'libexec'

if platform.system() == 'Linux':
    _LIB_SUFFIX = '.so'
elif platform.system() == 'Darwin':
    _LIB_SUFFIX = '.dylib'
elif platform.system() == 'Windows':
    _LIB_SUFFIX = '.dll'
else:
    _LOG.error('Unsupported platform.system(): %s', platform.system())
    sys.exit(1)


def _custom_find_library(name: str) -> str | None:
    """Search for shared libraries in non-standard locations."""
    search_paths: list[Path] = []

    # Add to search_paths starting with lowest priority locations.

    if platform.system() == 'Darwin':
        # libusb from homebrew
        homebrew_prefix = os.environ.get('HOMEBREW_PREFIX', '')
        if homebrew_prefix:
            homebrew_lib = Path(homebrew_prefix) / 'lib'
            homebrew_lib = homebrew_lib.expanduser().resolve()
            if homebrew_lib.is_dir():
                search_paths.append(homebrew_lib)

    # libusb from pkg-config
    pkg_config_bin = shutil.which('pkg-config')
    if pkg_config_bin:
        # pkg-config often prefixes libraries with 'lib', check both.
        for pkg_name in [f'lib{name}', name]:
            pkg_config_command = [pkg_config_bin, '--variable=libdir', pkg_name]
            process = subprocess.run(
                pkg_config_command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if process.returncode == 0:
                pkg_config_libdir = Path(
                    process.stdout.decode('utf-8', errors='ignore').strip()
                )
                if pkg_config_libdir.is_dir():
                    search_paths.append(pkg_config_libdir)
                    break

    # libusb provided by CIPD:
    pw_env = pigweed_environment()
    if _LIBUSB_CIPD_INSTALL_ENV_VAR in pw_env:
        cipd_lib = (
            Path(getattr(pw_env, _LIBUSB_CIPD_INSTALL_ENV_VAR))
            / _LIBUSB_CIPD_SUBDIR
        )
        if cipd_lib.is_dir():
            search_paths.append(cipd_lib)

    # libusb provided by Bazel
    try:
        # pylint: disable=import-outside-toplevel
        from python.runfiles import runfiles  # type: ignore

        r = runfiles.Create()
        assert r is not None
        r_location = r.Rlocation(
            f'libusb/libusb-1.0{_LIB_SUFFIX}',
            r.CurrentRepository(),
        )
        assert r_location is not None
        libusb_dir = os.path.dirname(r_location)
        search_paths.append(Path(libusb_dir))
    except ImportError:
        pass

    _LOG.debug('Potential shared library search paths:')
    for path in search_paths:
        _LOG.debug(path)

    # Search for shared libraries in search_paths
    for libdir in reversed(search_paths):
        lib_results = sorted(
            str(lib.resolve())
            for lib in libdir.iterdir()
            if name in lib.name and _LIB_SUFFIX in lib.suffixes
        )
        if lib_results:
            _LOG.debug('Using %s located at: %s', name, lib_results[-1])
            # Return the highest lexigraphically sorted lib version
            return lib_results[-1]

    # Fallback to pyusb default of calling ctypes.util.find_library.
    return ctypes_find_library(name)


def _device_port_path(device: usb.core.Device) -> str:
    """Returns the chain of ports that represent where a device is attached.

    Example:
        2:2:1
    """
    ports = device.port_numbers  # type: ignore[attr-defined]
    return ":".join([str(port) for port in ports])


def _libusb_raspberry_pi_devices(
    bus_filter: int | None,
    port_filter: str | None,
    include_picos: bool = True,
    include_debug_probes: bool = True,
) -> Iterable[usb.core.Device]:
    """Finds all Pi Pico-related USB devices."""
    devices_to_match: list[int] = []
    if include_picos:
        devices_to_match.extend(_PICO_DEVICE_IDS)
    if include_debug_probes:
        devices_to_match.append(_DEBUG_PROBE_DEVICE_ID)

    def _custom_match(d: usb.core.Device):
        if d.idVendor != _RASPBERRY_PI_VENDOR_ID:  # type: ignore[attr-defined]
            return False
        if d.idProduct not in devices_to_match:  # type: ignore[attr-defined]
            return False
        if bus_filter is not None and d.bus != bus_filter:
            return False
        if port_filter is not None and _device_port_path(d) != port_filter:
            return False

        return True

    return usb.core.find(
        find_all=True,
        custom_match=_custom_match,
        backend=libusb1.get_backend(find_library=_custom_find_library),
    )


@dataclass
class PicoBoardInfo:
    """Information about a connected Pi Pico board.

    NOTE: As a Pico board is flashed and reset, the USB address can change.
    Also, the bootloader has a different serial number than the regular
    application firmware. For this reason, this object does NOT cache or store
    the serial number, address, or USB device ID.
    """

    bus: int
    port: str
    serial_port: Optional[str]
    manufacturer: Optional[str]
    product: Optional[str]

    def _lookup_device(self) -> usb.core.Device:
        for device in _libusb_raspberry_pi_devices(
            bus_filter=self.bus, port_filter=self.port
        ):
            product_id = device.idProduct  # type: ignore[attr-defined]
            if product_id not in _ALL_DEVICE_IDS:
                _LOG.error(
                    'Unknown device type on bus %s port %s', self.bus, self.port
                )
            if _device_port_path(device) == self.port:
                return device
        raise ValueError(
            'No Pico found, it may have been disconnected or flashed with '
            'an incompatible application'
        )

    def current_address(self) -> int:
        """Queries this device for its USB address.

        WARNING: This is not necessarily stable, and may change whenever the
        board is reset or flashed.
        """
        return self._lookup_device().address

    def current_serial_number(self) -> str:
        """Queries this device for its USB serial number.

        WARNING: This is not necessarily stable, and may change whenever the
        board is reset or flashed.
        """
        return self._lookup_device().serial_number

    @staticmethod
    def vendor_id() -> int:
        return _RASPBERRY_PI_VENDOR_ID

    def is_debug_probe(self) -> bool:
        return isinstance(self, PicoDebugProbeBoardInfo)


@dataclass
class PicoDebugProbeBoardInfo(PicoBoardInfo):
    """Information about a connected Pi Debug Probe.

    Unlike a Pi Pico, a Pi Debug Probe has a stable serial number and device ID.
    """

    serial_number: str

    def current_serial_number(self) -> str:
        return self.serial_number

    @staticmethod
    def device_id() -> int:
        return _DEBUG_PROBE_DEVICE_ID


@dataclass
class _BoardSerialInfo:
    """Object that ties a serial number to a serial com port."""

    serial_port: str
    serial_number: str


@dataclass
class _BoardUsbInfo:
    """Object that ties a serial number to other USB device information.

    WARNING: This is private and ephemeral because many of these values are not
    necessarily stable as a Pico is flashed and reset. Use PicoBoardInfo or
    PicoDebugProbeBoardInfo for more stable representations of an attached Pico.
    """

    serial_number: str
    bus: int
    port: str
    product: str
    manufacturer: str
    vendor_id: int
    product_id: int

    @property
    def in_bootloader_mode(self) -> bool:
        return self.product_id in _PICO_BOOTLOADER_DEVICE_IDS

    @property
    def in_usb_serial_mode(self) -> bool:
        return self.product_id in _PICO_USB_SERIAL_DEVICE_IDS

    @property
    def is_debug_probe(self) -> bool:
        return self.product_id == _DEBUG_PROBE_DEVICE_ID

    def __repr__(self) -> str:
        return repr(asdict(self))


def _detect_pico_usb_info(
    bus_filter: int | None = None,
    port_filter: str | None = None,
    include_picos: bool = True,
    include_debug_probes: bool = True,
) -> dict[str, _BoardUsbInfo]:
    """Finds Raspberry Pi Pico devices and retrieves USB info for each one."""
    boards: dict[str, _BoardUsbInfo] = {}
    devices = _libusb_raspberry_pi_devices(
        bus_filter=bus_filter,
        port_filter=port_filter,
        include_picos=include_picos,
        include_debug_probes=include_debug_probes,
    )

    if not devices:
        return boards

    _LOG.debug('==> Detecting Raspberry Pi devices')
    for device in devices:
        try:
            serial_number = device.serial_number
        except ValueError as e:
            _LOG.warning(
                '  --> A connected device has an inaccessible '
                'serial number: %s',
                e,
            )
            continue

        board_usb_info = _BoardUsbInfo(
            serial_number=serial_number,
            bus=device.bus,
            port=_device_port_path(device),
            product=device.product,
            manufacturer=device.manufacturer,
            vendor_id=device.idVendor,  # type: ignore[attr-defined]
            product_id=device.idProduct,  # type: ignore[attr-defined]
        )

        if board_usb_info.in_usb_serial_mode:
            boards[serial_number] = board_usb_info
            _LOG.debug(
                '  --> Found a Pi Pico in USB serial mode: %s', board_usb_info
            )

        elif board_usb_info.in_bootloader_mode:
            boards[serial_number] = board_usb_info
            _LOG.debug(
                '  --> Found a Pi Pico in bootloader mode: %s', board_usb_info
            )

        elif board_usb_info.is_debug_probe:
            boards[serial_number] = board_usb_info
            _LOG.debug(
                '  --> Found Raspberry Pi debug probe: %s', board_usb_info
            )
            if device.bcdDevice < 0x201:  # type: ignore[attr-defined]
                _LOG.error(
                    'Reliable flashing and testing not possible due to '
                    'outdated Debug Probe firmware (%d.%d.%d). Update to '
                    'version 2.0.1 or later. See https://www.raspberrypi.com/'
                    'documentation/microcontrollers/debug-probe.html for '
                    'update instructions.',
                    (device.bcdDevice >> 8 & 0xF),  # type: ignore[attr-defined]
                    (device.bcdDevice >> 4 & 0xF),  # type: ignore[attr-defined]
                    (device.bcdDevice & 0xF),  # type: ignore[attr-defined]
                )

        else:
            _LOG.warning(
                '  --> Found unknown/incompatible Raspberry Pi: %s',
                board_usb_info,
            )

    return boards


def _detect_pico_serial_ports() -> dict[str, _BoardSerialInfo]:
    """Finds the serial com port associated with each Raspberry Pi Pico."""
    boards = {}
    all_devs = sorted(
        serial.tools.list_ports.comports(), key=lambda d: d.device
    )
    for dev in all_devs:
        _LOG.debug(
            'Detected serial port: device=%s, name=%s, description=%s, '
            'hwid=%s, vid=%s, pid=%s, serial=%s, location=%s, interface=%s',
            dev.device,
            dev.name,
            dev.description,
            dev.hwid,
            dev.vid,
            dev.pid,
            dev.serial_number,
            dev.location,
            dev.interface,
        )
        if dev.vid == _RASPBERRY_PI_VENDOR_ID and (
            dev.pid in _PICO_USB_SERIAL_DEVICE_IDS
            or dev.pid == _DEBUG_PROBE_DEVICE_ID
        ):
            serial_number = dev.serial_number
            if serial_number is None:
                _LOG.error('Found pico with no serial number')
                continue
            # Some devices (e.g. pw_rp2350) expose multiple
            # CDC-ACM interfaces. Keep the first (primary) port.
            if serial_number not in boards:
                boards[serial_number] = _BoardSerialInfo(
                    serial_port=dev.device,
                    serial_number=serial_number,
                )
    return boards


def board_from_usb_port(
    bus: int,
    port: str,
    include_picos: bool = True,
    include_debug_probes: bool = True,
) -> PicoDebugProbeBoardInfo | PicoBoardInfo:
    """Retrieves board info for the Pico at the specified USB bus and port.

    Args:
        bus: The USB bus that the requested Pico resides on.
        port: The chain of ports as a colon separated list of integers (e.g.
            '1:4:2:2') that the requested Pico resides on. This only performs
            exact matches.

    Returns:
      The board at the requested bus/port.
    """
    serial_devices = _detect_pico_serial_ports()
    pico_usb_info = _detect_pico_usb_info(
        include_picos=include_picos,
        include_debug_probes=include_debug_probes,
        bus_filter=bus,
        port_filter=port,
    )

    if not pico_usb_info:
        raise ValueError(f'No matching device found on bus {bus} port {port}')

    if len(pico_usb_info) > 1:
        raise ValueError(f'Multiple Picos found on bus {bus} port {port}')

    usb_info = next(iter(pico_usb_info.values()))

    serial_port = None
    if usb_info.serial_number in serial_devices:
        serial_port = serial_devices[usb_info.serial_number].serial_port

    if usb_info.is_debug_probe:
        return PicoDebugProbeBoardInfo(
            bus=usb_info.bus,
            port=usb_info.port,
            serial_port=serial_port,
            serial_number=usb_info.serial_number,
            product=usb_info.product,
            manufacturer=usb_info.manufacturer,
        )

    return PicoBoardInfo(
        bus=usb_info.bus,
        port=usb_info.port,
        serial_port=serial_port,
        product=usb_info.product,
        manufacturer=usb_info.manufacturer,
    )


def detect_boards(
    include_picos: bool = True, include_debug_probes: bool = False
) -> Sequence[PicoBoardInfo | PicoDebugProbeBoardInfo]:
    """Detects attached Raspberry Pi Pico boards in USB serial mode.

    Args:
        include_picos: Whether or not to include detected Raspberry Pi Picos in
            the list of enumerated devices.
        include_debug_probes: Whether or not to include detected Raspberry Pi
            debug probes in the list of enumerated devices.

    Returns:
      A list of all found boards.
    """
    serial_devices = _detect_pico_serial_ports()
    pico_usb_info = _detect_pico_usb_info(
        include_picos=include_picos,
        include_debug_probes=include_debug_probes,
    )
    boards: list[PicoBoardInfo | PicoDebugProbeBoardInfo] = []
    for serial_number, usb_info in pico_usb_info.items():
        if not include_debug_probes and usb_info.is_debug_probe:
            continue

        if not include_picos and not usb_info.is_debug_probe:
            continue

        serial_port = None
        if serial_number in serial_devices:
            serial_port = serial_devices[serial_number].serial_port

        if usb_info.is_debug_probe:
            boards.append(
                PicoDebugProbeBoardInfo(
                    bus=usb_info.bus,
                    port=usb_info.port,
                    serial_port=serial_port,
                    serial_number=usb_info.serial_number,
                    product=usb_info.product,
                    manufacturer=usb_info.manufacturer,
                )
            )
        elif serial_port or usb_info.in_bootloader_mode:
            boards.append(
                PicoBoardInfo(
                    bus=usb_info.bus,
                    port=usb_info.port,
                    serial_port=serial_port,
                    product=usb_info.product,
                    manufacturer=usb_info.manufacturer,
                )
            )

    return boards


def main():
    """Detects and then prints all attached Raspberry Pi Picos."""
    pw_cli.log.install(
        level=logging.DEBUG, use_color=True, hide_timestamp=False
    )

    boards = detect_boards(include_picos=True, include_debug_probes=True)
    if not boards:
        _LOG.info('No attached boards detected')
    for idx, board in enumerate(boards):
        _LOG.info('Board %d:', idx)
        _LOG.info('  %s', board)


if __name__ == '__main__':
    main()
