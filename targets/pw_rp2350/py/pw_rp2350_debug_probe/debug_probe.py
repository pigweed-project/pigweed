# Copyright 2026 The Pigweed Authors
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

"""pw_rp2350 debug probe driver."""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from enum import Enum, IntEnum
from typing import TYPE_CHECKING, Protocol

import usb.util  # type: ignore[import-untyped]

if TYPE_CHECKING:
    from typing import Self

_LOG = logging.getLogger(__name__)

_VENDOR_REQUEST = 0x50
_MAX_CONTROL_TRANSFER_BYTES = 256
_MAX_TARGET_INFO_BYTES = 4096
_MAX_COLOR_VALUE = 255
_DEFAULT_TIMEOUT_MS = 1000


class _RequestType(IntEnum):
    VENDOR_DEVICE_OUT = 0x40
    VENDOR_DEVICE_IN = 0xC0


class _Command(IntEnum):
    HELLO = 0
    SET_TARGET = 1
    GET_TARGET_INFO = 2
    SET_LED = 3


class SwdTarget(IntEnum):
    """SWD target selection for the debug probe."""

    DUT = 0
    SIM = 1


class Capability(Enum):
    """Hardware capabilities supported by a test target."""

    UART = 0
    SPI = 1
    I2C = 2
    DIGITAL_IO = 3
    ANALOG = 4


@dataclass
class TestTargetInfo:
    """Metadata describing the connected test target."""

    name: str
    mcu: str
    capabilities: list[Capability]


class UsbBuffer(Protocol):
    """Protocol for a byte-convertible buffer returned by pyusb."""

    def tobytes(self) -> bytes:
        """Returns the buffer contents as raw bytes."""


class UsbDevice(Protocol):
    """Protocol for a USB device supporting control transfers."""

    # pylint: disable=invalid-name
    def ctrl_transfer(  # noqa: PLR0913, PLR0917
        self,
        bmRequestType: int,  # noqa: N803
        bRequest: int,  # noqa: N803
        wValue: int = 0,  # noqa: N803
        wIndex: int = 0,  # noqa: N803
        data_or_wLength: bytes | int | None = None,  # noqa: N803
        timeout: int | None = None,
    ) -> UsbBuffer | int:
        """Performs a USB control transfer."""


def _parse_target_info_dict(info: object) -> TestTargetInfo:
    """Extracts and validates target-info fields from a decoded JSON object."""
    if not isinstance(info, dict):
        msg = 'Expected a JSON object'
        raise TypeError(msg)

    name = info['name']
    mcu = info['mcu']
    raw_capabilities = info['capabilities']
    if (
        not isinstance(name, str)
        or not isinstance(mcu, str)
        or not isinstance(raw_capabilities, list)
    ):
        msg = 'Unexpected target info field types'
        raise TypeError(msg)

    capabilities: list[Capability] = []
    for cap in raw_capabilities:
        capability = Capability.__members__.get(cap.upper())
        if capability is None:
            _LOG.warning('Unknown target capability: %s', cap)
            continue
        capabilities.append(capability)

    return TestTargetInfo(
        name=name,
        mcu=mcu,
        capabilities=capabilities,
    )


def _parse_target_info(info: object) -> TestTargetInfo:
    """Validates and converts a decoded target-info JSON value."""
    try:
        return _parse_target_info_dict(info)
    except (KeyError, TypeError, AttributeError) as err:
        msg = f'Invalid target info payload: {err}'
        raise ValueError(msg) from err


class DebugProbe:
    """Controller for the pw_rp2350 debug probe hardware."""

    def __init__(
        self,
        device: UsbDevice,
        timeout_ms: int = _DEFAULT_TIMEOUT_MS,
    ) -> None:
        """Initializes the debug probe controller with a USB device."""
        self._device = device
        self._timeout_ms = timeout_ms

    def _ctrl_transfer(
        self,
        request_type: _RequestType,
        command: _Command,
        *,
        value: int = 0,
        data_or_length: bytes | int | None = None,
        timeout_ms: int | None = None,
    ) -> UsbBuffer | int:
        """Performs a vendor USB control transfer with a timeout."""
        return self._device.ctrl_transfer(
            bmRequestType=request_type,
            bRequest=_VENDOR_REQUEST,
            wValue=value,
            wIndex=command,
            data_or_wLength=data_or_length,
            timeout=self._timeout_ms if timeout_ms is None else timeout_ms,
        )

    def _ctrl_transfer_in(self, command: _Command, max_length: int) -> bytes:
        """Reads raw bytes from a vendor IN control transfer."""
        result = self._ctrl_transfer(
            _RequestType.VENDOR_DEVICE_IN,
            command,
            data_or_length=max_length,
        )
        if isinstance(result, int):
            msg = 'Expected byte buffer from IN control transfer'
            raise TypeError(msg)
        return result.tobytes()

    def close(self) -> None:
        """Releases underlying USB device resources."""
        usb.util.dispose_resources(self._device)

    def __enter__(self) -> Self:
        """Enters the runtime context for the debug probe."""
        return self

    def __exit__(self, *args: object) -> None:
        """Exits the runtime context and releases USB device resources."""
        self.close()

    def hello(self) -> str:
        """Queries the debug probe greeting payload."""
        raw = self._ctrl_transfer_in(
            _Command.HELLO, _MAX_CONTROL_TRANSFER_BYTES
        )
        return raw.decode('utf-8').rstrip('\x00')

    def set_target(self, target: SwdTarget) -> None:
        """Selects the active SWD target (DUT or SIM) on the debug probe."""
        self._ctrl_transfer(
            _RequestType.VENDOR_DEVICE_OUT,
            _Command.SET_TARGET,
            value=target,
        )

    def get_target_info(self) -> TestTargetInfo:
        """Reads and parses metadata about the connected test target."""
        raw = self._ctrl_transfer_in(
            _Command.GET_TARGET_INFO, _MAX_TARGET_INFO_BYTES
        )
        raw_json = raw.decode('utf-8').rstrip('\x00')
        return _parse_target_info(json.loads(raw_json))

    def set_rgb_led(self, r: int, g: int, b: int) -> None:
        """Sets the debug probe RGB status LED color."""
        for channel, value in (('r', r), ('g', g), ('b', b)):
            if not 0 <= value <= _MAX_COLOR_VALUE:
                msg = (
                    f'{channel} must be in range 0..{_MAX_COLOR_VALUE}, '
                    f'got {value}'
                )
                raise ValueError(msg)

        self._ctrl_transfer(
            _RequestType.VENDOR_DEVICE_OUT,
            _Command.SET_LED,
            data_or_length=bytes((r, g, b)),
        )
