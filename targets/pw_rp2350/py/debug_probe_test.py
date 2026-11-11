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

"""Tests for pw_rp2350_debug_probe.debug_probe."""

from __future__ import annotations

import json
import unittest
from array import array
from enum import IntEnum
from typing import Any
from unittest.mock import MagicMock, patch

import usb.core  # type: ignore[import-untyped]
import usb.util  # type: ignore[import-untyped]
from pw_rp2350_debug_probe.debug_probe import (
    Capability,
    DebugProbe,
    SwdTarget,
    TestTargetInfo,
)
from rp2040_utils.device_detector import PicoBoardInfo


class FakeUsbDevice:
    """Lightweight fake USB device implementing UsbDevice for testing."""

    def __init__(self, response: bytes = b'') -> None:
        """Initializes the fake USB device with an optional response payload."""
        self.calls: list[dict[str, Any]] = []
        self._response = array('B', response)

    # pylint: disable=invalid-name
    def ctrl_transfer(  # noqa: PLR0913, PLR0917
        self,
        bmRequestType: int,  # noqa: N803
        bRequest: int,  # noqa: N803
        wValue: int = 0,  # noqa: N803
        wIndex: int = 0,  # noqa: N803
        data_or_wLength: bytes | int | None = None,  # noqa: N803
        timeout: int | None = None,
    ) -> array[int] | int:
        """Records the control transfer parameters and returns canned data."""
        self.calls.append(
            {
                'bmRequestType': bmRequestType,
                'bRequest': bRequest,
                'wValue': wValue,
                'wIndex': wIndex,
                'data_or_wLength': data_or_wLength,
                'timeout': timeout,
            }
        )
        if bmRequestType & 0x80:
            return self._response
        if isinstance(data_or_wLength, bytes):
            return len(data_or_wLength)
        return 0


class PicoBoardInfoTest(unittest.TestCase):
    """Tests for PicoBoardInfo USB device lookup."""

    def test_usb_device_delegates_to_lookup_device(self) -> None:
        """Verifies `PicoBoardInfo.usb_device` delegates to `_lookup_device`."""
        board = PicoBoardInfo(
            bus=1,
            port='1:4',
            serial_port='/dev/ttyACM0',
            manufacturer='Raspberry Pi',
            product='Debug Probe (CMSIS-DAP)',
        )
        mock_device = MagicMock(spec=usb.core.Device)
        with patch.object(
            PicoBoardInfo, '_lookup_device', return_value=mock_device
        ) as mock_lookup:
            device = board.usb_device()

        mock_lookup.assert_called_once_with()
        self.assertIs(device, mock_device)
        self.assertIsNotNone(DebugProbe(device))


class DebugProbeTest(unittest.TestCase):
    """Tests for DebugProbe control transfers, validation, and lifecycle."""

    def test_hello_returns_decoded_str_and_verifies_transfer(self) -> None:
        """Verifies `hello` decodes UTF-8 greeting bytes and passes params."""
        fake_dev = FakeUsbDevice(b'hello')
        probe = DebugProbe(fake_dev)

        greeting = probe.hello()

        self.assertIsInstance(greeting, str)
        self.assertEqual(greeting, 'hello')
        self.assertEqual(
            fake_dev.calls,
            [
                {
                    'bmRequestType': 0xC0,
                    'bRequest': 0x50,
                    'wValue': 0,
                    'wIndex': 0,
                    'data_or_wLength': 256,
                    'timeout': 1000,
                }
            ],
        )

    def test_hello_strips_trailing_nul_bytes(self) -> None:
        """Verifies `hello` strips trailing NUL padding bytes."""
        fake_dev = FakeUsbDevice(b'hello\x00\x00')
        probe = DebugProbe(fake_dev)

        self.assertEqual(probe.hello(), 'hello')

    def test_custom_timeout_is_passed_to_ctrl_transfer(self) -> None:
        """Verifies custom `timeout_ms` is forwarded to `ctrl_transfer`."""
        fake_dev = FakeUsbDevice(b'hello')
        probe = DebugProbe(fake_dev, timeout_ms=2500)

        probe.hello()

        self.assertEqual(fake_dev.calls[0]['timeout'], 2500)

    def test_swd_target_int_enum_values_and_set_target(self) -> None:
        """Verifies `SwdTarget` IntEnum values and `set_target` transfers."""
        self.assertIsInstance(SwdTarget.DUT, IntEnum)
        self.assertIsInstance(SwdTarget.SIM, IntEnum)
        self.assertEqual(SwdTarget.DUT, 0)
        self.assertEqual(SwdTarget.SIM, 1)

        fake_dev = FakeUsbDevice()
        probe = DebugProbe(fake_dev)
        probe.set_target(SwdTarget.DUT)
        probe.set_target(SwdTarget.SIM)

        self.assertEqual(
            fake_dev.calls,
            [
                {
                    'bmRequestType': 0x40,
                    'bRequest': 0x50,
                    'wValue': 0,
                    'wIndex': 1,
                    'data_or_wLength': None,
                    'timeout': 1000,
                },
                {
                    'bmRequestType': 0x40,
                    'bRequest': 0x50,
                    'wValue': 1,
                    'wIndex': 1,
                    'data_or_wLength': None,
                    'timeout': 1000,
                },
            ],
        )

    def test_get_target_info_parses_valid_json_capabilities(self) -> None:
        """Verifies `get_target_info` parses target JSON and capabilities."""
        payload = json.dumps(
            {
                'name': 'Pico 2 Test Target',
                'mcu': 'RP2350',
                'capabilities': ['uart', 'SPI', 'i2c', 'digital_io', 'analog'],
            }
        ).encode('utf-8')
        fake_dev = FakeUsbDevice(payload)
        probe = DebugProbe(fake_dev)

        info = probe.get_target_info()

        self.assertEqual(
            info,
            TestTargetInfo(
                name='Pico 2 Test Target',
                mcu='RP2350',
                capabilities=[
                    Capability.UART,
                    Capability.SPI,
                    Capability.I2C,
                    Capability.DIGITAL_IO,
                    Capability.ANALOG,
                ],
            ),
        )
        self.assertEqual(
            fake_dev.calls,
            [
                {
                    'bmRequestType': 0xC0,
                    'bRequest': 0x50,
                    'wValue': 0,
                    'wIndex': 2,
                    'data_or_wLength': 4096,
                    'timeout': 1000,
                }
            ],
        )

    def test_get_target_info_ignores_and_logs_unknown_capabilities(
        self,
    ) -> None:
        """Verifies unknown capability names are logged and skipped."""
        payload = json.dumps(
            {
                'name': 'Future Board',
                'mcu': 'RP2350B',
                'capabilities': ['uart', 'can_bus', 'jtag', 'i2c'],
            }
        ).encode('utf-8')
        fake_dev = FakeUsbDevice(payload)
        probe = DebugProbe(fake_dev)

        with self.assertLogs(
            'pw_rp2350_debug_probe.debug_probe', level='WARNING'
        ) as log_ctx:
            info = probe.get_target_info()

        self.assertEqual(
            info,
            TestTargetInfo(
                name='Future Board',
                mcu='RP2350B',
                capabilities=[Capability.UART, Capability.I2C],
            ),
        )
        self.assertEqual(len(log_ctx.output), 2)
        self.assertIn('Unknown target capability: can_bus', log_ctx.output[0])
        self.assertIn('Unknown target capability: jtag', log_ctx.output[1])

    def test_get_target_info_raises_value_error_on_malformed_payload(
        self,
    ) -> None:
        """Verifies malformed JSON payloads raise ValueError uniformly."""
        malformed_payloads = [
            b'\xff\xfe',
            b'{not_valid_json',
            b'[]',
            b'123',
            b'{"name": "Board"}',
            b'{"name": "Board", "mcu": "RP2350"}',
            b'{"name": 123, "mcu": "RP2350", "capabilities": []}',
            b'{"name": "Board", "mcu": null, "capabilities": []}',
            b'{"name": "Board", "mcu": "RP2350", "capabilities": "uart"}',
            b'{"name": "Board", "mcu": "RP2350", "capabilities": [123]}',
            b'{"name": "Board", "mcu": "RP2350", "capabilities": [null]}',
        ]
        for payload in malformed_payloads:
            with self.subTest(payload=payload):
                probe = DebugProbe(FakeUsbDevice(payload))
                with self.assertRaises(ValueError):  # noqa: PT027
                    probe.get_target_info()

    def test_set_rgb_led_sends_bytes_payload(self) -> None:
        """Verifies `set_rgb_led` transmits a 3-byte RGB payload."""
        fake_dev = FakeUsbDevice()
        probe = DebugProbe(fake_dev)

        probe.set_rgb_led(10, 20, 30)

        self.assertEqual(
            fake_dev.calls,
            [
                {
                    'bmRequestType': 0x40,
                    'bRequest': 0x50,
                    'wValue': 0,
                    'wIndex': 3,
                    'data_or_wLength': bytes((10, 20, 30)),
                    'timeout': 1000,
                }
            ],
        )
        self.assertIs(type(fake_dev.calls[0]['data_or_wLength']), bytes)

    def test_set_rgb_led_accepts_boundary_values(self) -> None:
        """Verifies `set_rgb_led` accepts channel boundary values 0 and 255."""
        fake_dev = FakeUsbDevice()
        probe = DebugProbe(fake_dev)

        probe.set_rgb_led(0, 0, 0)
        probe.set_rgb_led(255, 255, 255)

        self.assertEqual(len(fake_dev.calls), 2)
        self.assertEqual(fake_dev.calls[0]['data_or_wLength'], bytes((0, 0, 0)))
        self.assertEqual(
            fake_dev.calls[1]['data_or_wLength'], bytes((255, 255, 255))
        )

    def test_set_rgb_led_raises_value_error_for_out_of_range_channels(
        self,
    ) -> None:
        """Verifies `set_rgb_led` raises `ValueError` outside 0..255."""
        invalid_cases = [
            ('r', -1, 0, 0),
            ('r', 256, 0, 0),
            ('g', 0, -1, 0),
            ('g', 0, 256, 0),
            ('b', 0, 0, -1),
            ('b', 0, 0, 256),
        ]
        for channel, r, g, b in invalid_cases:
            with self.subTest(channel=channel, r=r, g=g, b=b):
                fake_dev = FakeUsbDevice()
                probe = DebugProbe(fake_dev)
                with self.assertRaisesRegex(ValueError, channel):  # noqa: PT027
                    probe.set_rgb_led(r, g, b)
                self.assertEqual(fake_dev.calls, [])

    def test_close_disposes_usb_resources(self) -> None:
        """Verifies `close` calls `usb.util.dispose_resources`."""
        fake_dev = FakeUsbDevice()
        probe = DebugProbe(fake_dev)

        with patch.object(usb.util, 'dispose_resources') as mock_dispose:
            probe.close()

        mock_dispose.assert_called_once_with(fake_dev)
        self.assertEqual(fake_dev.calls, [])

    def test_context_manager_closes_on_exit(self) -> None:
        """Verifies exiting a `with DebugProbe(...)` block disposes handles."""
        fake_dev = FakeUsbDevice()

        with (
            patch.object(usb.util, 'dispose_resources') as mock_dispose,
            DebugProbe(fake_dev) as probe,
        ):
            self.assertIsInstance(probe, DebugProbe)
            mock_dispose.assert_not_called()

        mock_dispose.assert_called_once_with(fake_dev)

    def test_context_manager_closes_on_exception(self) -> None:
        """Verifies `DebugProbe` disposes handles even when body raises."""
        fake_dev = FakeUsbDevice()

        with (
            patch.object(usb.util, 'dispose_resources') as mock_dispose,
            self.assertRaises(RuntimeError),  # noqa: PT027
            DebugProbe(fake_dev),
        ):
            raise RuntimeError

        mock_dispose.assert_called_once_with(fake_dev)


if __name__ == '__main__':
    unittest.main()
