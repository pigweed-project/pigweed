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
"""Launch a simulated device subprocess under pw_console."""

import argparse
from collections.abc import Sequence
from pathlib import Path
import subprocess
import sys
from types import ModuleType

from pw_system.console import console, get_parser
from pw_system.device_connection import DeviceConnection


def _parse_args() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--sim-binary',
        type=Path,
        help='Path to a simulated device binary to run on the host',
    )
    return parser.parse_known_args()


def launch_sim(
    sim_binary: Path,
    remaining_args: list[str],
    compiled_protos: Sequence[ModuleType] | None = None,
    device_connection: DeviceConnection | None = None,
) -> int:
    """Launches a host-device-sim binary, and attaches a console to it."""
    if '-s' not in remaining_args and '--socket-addr' not in remaining_args:
        remaining_args.extend(['--socket-addr', 'default'])

    # Pass the remaining arguments on to pw_system console.
    console_args = get_parser().parse_args(remaining_args)

    sim_process = subprocess.Popen(
        sim_binary,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )

    try:
        retval = console(
            **vars(console_args),
            compiled_protos=compiled_protos,
            device_connection=device_connection,
        )
    finally:
        sim_process.terminate()

    return retval


def main(
    compiled_protos: Sequence[ModuleType] | None = None,
    device_connection: DeviceConnection | None = None,
) -> int:
    sim_args, remaining_args = _parse_args()
    return launch_sim(
        **vars(sim_args),
        remaining_args=remaining_args,
        compiled_protos=compiled_protos,
        device_connection=device_connection,
    )


# TODO(b/353327855): Deprecated function, remove when no longer used.
def main_with_compiled_protos(
    compiled_protos: Sequence[ModuleType] | None = None,
    device_connection: DeviceConnection | None = None,
) -> int:
    return main(
        compiled_protos=compiled_protos,
        device_connection=device_connection,
    )


if __name__ == '__main__':
    sys.exit(main())
