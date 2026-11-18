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
"""This file provides a basic host-side runner class for RPC benchmarking"""
import argparse
import logging
from types import ModuleType
from typing import Any

from pw_protobuf_protos import common_pb2
from pw_rpc import echo_pb2, benchmark_pb2
from pw_rpc.benchmark import benchmark
from pw_rpc.benchmark.benchmark import BenchmarkOptions
from pw_system import device as pw_device
from pw_system.device_connection import (
    create_device_serial_or_socket_connection,
)

default_protos: list[ModuleType] = [common_pb2, echo_pb2, benchmark_pb2]


class Runner:
    """Provides a default runner to execute the benchmark utilities.

    This class is a convenience class to enable a user to execute the default
    tests provided by ``pw_rpc``'s ``benchmark.py`` file.  Users are generally
    expected to replace this runner with their own, customized version once
    they verify the default tools work.
    """

    def __enter__(self) -> Any:
        """Manages any setup required for the runner in a with context.

        Args:
            None.

        Returns:
            None - Throws exceptions on error.
        """
        return self

    def __init__(
        self,
        args: argparse.Namespace,
        logger: logging.Logger,
        benchmark_options: BenchmarkOptions = BenchmarkOptions(),
        device: pw_device.Device | None = None,
    ) -> None:
        """Creates a Runner object for interacting with pw_rpc benchmark.

        This must be called in a `with` context!

        Args:
            args: An arg list for the CLI arg parser.
            benchmark_options : what options to set for the benchmarker.
            device: An optional device connection to use. Defaults to
                a default (sim) connection if None.
            logger: Where to log package data.

        Returns:
            None: Throws exceptions on error.
        """

        self.logger = logger

        if device is None:
            connection = create_device_serial_or_socket_connection(
                device=args.device,
                baudrate=args.baudrate,
                token_databases=args.token_databases,
                compiled_protos=default_protos,
                socket_addr=args.socket_addr,
                ticks_per_second=args.ticks_per_second,
                serial_debug=args.serial_debug,
                rpc_logging=args.rpc_logging,
                hdlc_encoding=args.hdlc_encoding,
                channel_id=args.channel_id,
                device_tracing=args.device_tracing,
            )

            # Needing to do this is a quirk of how the device connection class
            # is defined.
            # Note: Add a weakref.finalizer if the runner class is changed to
            # function outside of a context manager (with statement).
            self.device = connection.__enter__()

        else:
            self.device = device
        self.rpcs = self.device.rpcs
        self.uut = benchmark.Benchmark(
            rpcs=self.rpcs, options=benchmark_options
        )

    def execute_all(self) -> int:
        """Runs all tests available to pw_rpc's benchmark tools.

        Args:
            None.

        Returns:
            None.
        """

        # TODO: https://pwbug.dev/380584579 - make an iterator for all the
        # benchmark tests.
        result = self.uut.measure_rpc_goodput()
        result.print_results()
        result = self.uut.measure_rpc_goodput_statistics()
        result.print_results()
        buffer_size_result = self.uut.find_max_echo_buffer_size()
        buffer_size_result.print_results()

        return 0

    def __exit__(self, exc_type, exc_value, exc_traceback) -> None:
        """Ensures all pw_rpc benchmark objects have been properly deleted.

        Args:
            None.

        Returns:
            None: Throws exceptions on error.
        """

        if self.device:
            self.device.__exit__()
