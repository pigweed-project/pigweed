#!/usr/bin/env python3
# Copyright 2024 The Pigweed Authors
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
"""Tests for snapshot processing."""

import base64
import textwrap
import unittest

from pw_log.proto import log_pb2
from pw_metric_proto import metric_service_pb2
from pw_tokenizer import detokenize, tokens

from pw_snapshot.processor import process_snapshot
from pw_snapshot_protos import snapshot_pb2

_SNAPSHOT_HEADER = """
        ____ _       __    _____ _   _____    ____  _____ __  ______  ______
       / __ \\ |     / /   / ___// | / /   |  / __ \\/ ___// / / / __ \\/_  __/
      / /_/ / | /| / /    \\__ \\/  |/ / /| | / /_/ /\\__ \\/ /_/ / / / / / /
     / ____/| |/ |/ /    ___/ / /|  / ___ |/ ____/___/ / __  / /_/ / / /
    /_/     |__/|__/____/____/_/ |_/_/  |_/_/    /____/_/ /_/\\____/ /_/
                  /_____/

"""

_RISCV_EXPECTED_SNAPSHOT = (
    _SNAPSHOT_HEADER
    + """
Snapshot capture reason:
    Example Reason

Reason token:      0x6d617845
Project name:      example_project
Device:            hyper-fast-gshoe
CPU Arch:          RV32I
Device FW version: gShoe-debug-1.2.1-6f23412b+
Snapshot UUID:     00000001

All registers:
mepc       0x20000001
mcause     0x20000002
mstatus    0x20000003
"""
)

_ARM_EXPECTED_SNAPSHOT = (
    _SNAPSHOT_HEADER
    + """
Snapshot capture reason:
    Example Reason

Reason token:      0x6d617845
Project name:      example_project
Device:            hyper-fast-gshoe
CPU Arch:          ARMV7M
Device FW version: gShoe-debug-1.2.1-6f23412b+
Snapshot UUID:     00000001

Exception caused by a unknown exception.

No active Crash Fault Status Register (CFSR) fields.

All registers:
pc         0x20000001
lr         0x20000002
psr        0x20000003
"""
)

_TOKEN_DB = tokens.Database()


def _make_token(string: str) -> int:
    """Tokenizes the given string and adds it to the database.

    Returns: The token value.
    """
    token = tokens.pw_tokenizer_65599_hash(string)
    _TOKEN_DB.add([tokens.TokenizedStringEntry(token, string)])
    return token


_LOG_MESSAGE_TOKEN = _make_token("A tokenized log message!")

_BATTERY_TOKEN = _make_token("battery")
_INTERRUPTS_TOKEN = _make_token("interrupts")
_SPI0_TOKEN = _make_token("spi0")
_TEMPERATURE_TOKEN = _make_token("temperature")
_UART0_TOKEN = _make_token("uart0")
_VOLTAGE_TOKEN = _make_token("voltage")
_RAM_TOKEN = _make_token("t-ram")


_TEST_LOGS = [
    log_pb2.LogEntry(message=b"Basic"),
    log_pb2.LogEntry(
        message=_LOG_MESSAGE_TOKEN.to_bytes(length=4, byteorder="little"),
    ),
    log_pb2.LogEntry(
        message=b"Hello, world!",
        line_level=(1234 << 3) | 4,
        timestamp=2745587123456,
        module=b"MYMOD",
        file=b"dispatcher.c",
        thread=b"Dispatcher",
    ),
]

_TEST_METRICS = [
    metric_service_pb2.Metric(
        token_path=[_BATTERY_TOKEN, _TEMPERATURE_TOKEN],
        as_float=38.0,
    ),
    metric_service_pb2.Metric(
        token_path=[_BATTERY_TOKEN, _VOLTAGE_TOKEN],
        as_float=2.5,
    ),
    metric_service_pb2.Metric(
        token_path=[_INTERRUPTS_TOKEN, _SPI0_TOKEN],
        as_int=11234,
    ),
    metric_service_pb2.Metric(
        token_path=[_INTERRUPTS_TOKEN, _UART0_TOKEN],
        as_int=22345,
    ),
]


class ProcessorTest(unittest.TestCase):
    """Tests that the metadata processor produces expected results."""

    maxDiff = None

    def test_riscv_process_snapshot(self):
        """Test processing snapshot of a RISCV CPU"""

        snapshot = base64.b64decode(
            "ggFYCg5FeGFtcGxlIFJlYXNvbhoPZXhhbXBs"
            "ZV9wcm9qZWN0IhtnU2hvZS1kZWJ1Zy0xLjIu"
            "MS02ZjIzNDEyYisyEGh5cGVyLWZhc3QtZ3No"
            "b2U6BAAAAAFABaIBEgiBgICAAhCCgICAAhiD"
            "gICAAg=="
        )

        output = process_snapshot(snapshot)
        self.assertEqual(output, _RISCV_EXPECTED_SNAPSHOT)

    def test_cortexm_process_snapshot(self):
        """Test processing snapshot of a ARM CPU"""

        snapshot = base64.b64decode(
            "ggFYCg5FeGFtcGxlIFJlYXNvbhoPZXhhbXBsZV9wc"
            "m9qZWN0IhtnU2hvZS1kZWJ1Zy0xLjIuMS02ZjIzND"
            "EyYisyEGh5cGVyLWZhc3QtZ3Nob2U6BAAAAAFAAqI"
            "BEgiBgICAAhCCgICAAhiDgICAAg=="
        )

        output = process_snapshot(snapshot)
        self.assertEqual(output, _ARM_EXPECTED_SNAPSHOT)

    def test_process_snapshot_with_logs(self):
        snapshot = snapshot_pb2.Snapshot(logs=_TEST_LOGS)
        expected_output = _SNAPSHOT_HEADER + textwrap.dedent(
            """
            Logs:
              Basic
              A tokenized log message!
              ERR MYMOD 00:45:45.587123 Hello, world! dispatcher.c:1234
            """
        )

        detokenizer = detokenize.Detokenizer(_TOKEN_DB)
        output = process_snapshot(
            snapshot.SerializeToString(), detokenizer=detokenizer
        )
        self.assertEqual(output, expected_output)

    def test_process_snapshot_with_no_log_processor(self):
        """Verify process_snapshot() supports process_logs=None."""
        snapshot = snapshot_pb2.Snapshot(logs=_TEST_LOGS)
        expected_output = _SNAPSHOT_HEADER

        output = process_snapshot(
            snapshot.SerializeToString(),
            process_logs=None,
        )
        self.assertEqual(output, expected_output)

    def test_process_snapshot_with_metrics(self):
        snapshot = snapshot_pb2.Snapshot(metrics=_TEST_METRICS)
        expected_output = _SNAPSHOT_HEADER + textwrap.dedent(
            """
            Metrics:
            {
              "battery": {
                "temperature": 38.0,
                "voltage": 2.5
              },
              "interrupts": {
                "spi0": 11234,
                "uart0": 22345
              }
            }
            """
        )

        detokenizer = detokenize.Detokenizer(_TOKEN_DB)
        output = process_snapshot(
            snapshot.SerializeToString(), detokenizer=detokenizer
        )
        self.assertEqual(output, expected_output)

    def test_process_snapshot_with_memory_regions(self):
        snapshot = snapshot_pb2.Snapshot(
            memory_regions=[
                snapshot_pb2.MemoryRegion(
                    name=b"main_ram",
                    address=0x20000000,
                    data=b"\x01\x02\x03\x04\x05\x06\x07\x08",
                ),
                snapshot_pb2.MemoryRegion(
                    address=0x08000000,
                    data=b"\xde\xad\xbe\xef",
                ),
            ]
        )
        expected_output = _SNAPSHOT_HEADER + textwrap.dedent(
            """
            Memory Regions:
              20000000 (main_ram) (8 bytes)
              08000000 (4 bytes)
            """
        )

        output = process_snapshot(snapshot.SerializeToString())
        self.assertEqual(output, expected_output)

    def test_process_snapshot_with_memory_region_processor(self):
        snapshot = snapshot_pb2.Snapshot(
            memory_regions=[
                snapshot_pb2.MemoryRegion(
                    name=b"main_ram",
                    address=0x20000000,
                    data=b"\x01\x02\x03\x04\x05\x06\x07\x08",
                ),
            ]
        )

        def mock_processor(region: snapshot_pb2.MemoryRegion) -> str:
            return f"Processed {len(region.data)} bytes at {region.address:08x}"

        expected_output = _SNAPSHOT_HEADER + textwrap.dedent(
            """
            Memory Regions:
              20000000 (main_ram) (8 bytes)
                Processed 8 bytes at 20000000
            """
        )

        output = process_snapshot(
            snapshot.SerializeToString(), memory_region_processor=mock_processor
        )
        self.assertEqual(output, expected_output)

    def test_process_snapshot_with_tokenized_memory_regions(self):
        snapshot = snapshot_pb2.Snapshot(
            memory_regions=[
                snapshot_pb2.MemoryRegion(
                    name=_RAM_TOKEN.to_bytes(length=4, byteorder="little"),
                    address=0x30000000,
                    data=b"\x01\x02",
                ),
            ]
        )
        expected_output = _SNAPSHOT_HEADER + textwrap.dedent(
            """
            Memory Regions:
              30000000 (t-ram) (2 bytes)
            """
        )

        detokenizer = detokenize.Detokenizer(_TOKEN_DB)
        output = process_snapshot(
            snapshot.SerializeToString(), detokenizer=detokenizer
        )
        self.assertEqual(output, expected_output)


if __name__ == '__main__':
    unittest.main()
