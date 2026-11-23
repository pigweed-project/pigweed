#!/usr/bin/env python3
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
"""Detects attached STM32 Nucleo boards connected via ST-LINK USB."""

from __future__ import annotations

from dataclasses import dataclass
import logging
import re

import serial.tools.list_ports  # type: ignore[import-untyped]

_ST_VENDOR_ID = 0x0483
VALID_SERIAL_RE = re.compile(r'^[A-Za-z0-9_-]+$')
_STLINK_PRODUCT_IDS = frozenset(
    {
        0x3748,  # ST-LINK/V2
        0x374B,  # ST-LINK/V2-1 (Nucleo-64 default)
        0x374E,  # ST-LINK/V3
        0x374F,  # ST-LINK/V3
        0x3752,  # ST-LINK/V2-1
        0x3753,  # ST-LINK/V3
        0x3754,  # ST-LINK/V3
    }
)

_LOG = logging.getLogger(__name__)


@dataclass(frozen=True)
class BoardInfo:
    """Information about a connected STM32 Nucleo board."""

    dev_name: str
    serial_number: str | None
    vendor_id: int = _ST_VENDOR_ID
    product_id: int = 0x374B

    def probe_selector(self) -> str:
        """Returns the probe-rs `--probe` selector string."""
        base = f'{self.vendor_id:04x}:{self.product_id:04x}'
        if self.serial_number:
            return f'{base}:{self.serial_number}'
        return base


def _port_sort_key(port: object) -> tuple[str, str]:
    location = getattr(port, 'location', None)
    device = getattr(port, 'device', '')
    return (location if isinstance(location, str) else '', str(device))


def detect_boards() -> list[BoardInfo]:
    """Detects attached ST-LINK boards exposing a Virtual COM Port."""
    boards: list[BoardInfo] = []
    seen_serials: set[str] = set()
    for dev in sorted(serial.tools.list_ports.comports(), key=_port_sort_key):
        if dev.vid == _ST_VENDOR_ID and dev.pid in _STLINK_PRODUCT_IDS:
            serial_num = dev.serial_number
            if serial_num is not None and not VALID_SERIAL_RE.fullmatch(
                serial_num
            ):
                _LOG.warning(
                    'Ignoring invalid USB serial descriptor on %s', dev.device
                )
                serial_num = None
            if serial_num is not None:
                if serial_num in seen_serials:
                    continue
                seen_serials.add(serial_num)
            boards.append(
                BoardInfo(
                    dev_name=dev.device,
                    serial_number=serial_num,
                    vendor_id=dev.vid,
                    product_id=dev.pid,
                )
            )
    return boards


def main() -> None:
    """Detects and logs attached STM32 Nucleo boards."""
    import pw_cli.log  # pylint: disable=import-outside-toplevel

    pw_cli.log.install()
    boards = detect_boards()
    if not boards:
        _LOG.info('No attached STM32 Nucleo boards detected')
        return
    for idx, board in enumerate(boards):
        _LOG.info(
            'Board %d: port=%s probe=%s',
            idx,
            board.dev_name,
            board.probe_selector(),
        )


if __name__ == '__main__':
    main()
