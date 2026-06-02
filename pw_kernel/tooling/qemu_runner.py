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
"""Utility to invoke qemu and pipe the output through the detokenizer.
"""

import argparse
import logging
import os
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path
from pw_tokenizer import detokenize

_LOG = logging.getLogger(__name__)
_LOG.setLevel(logging.INFO)

try:
    import qemu.qemu_system_arm  # type: ignore
    import qemu.qemu_system_riscv32  # type: ignore
    from python.runfiles import runfiles  # type: ignore

    r = runfiles.Create()
    assert r is not None
    _QEMU_ARM = r.Rlocation(
        *qemu.qemu_system_arm.RLOCATION,
    )
    _QEMU_RISCV32 = r.Rlocation(*qemu.qemu_system_riscv32.RLOCATION)
except ImportError:
    _LOG.fatal("runfiles could not find qemu")

# Keep mypy happy
assert _QEMU_ARM is not None
assert _QEMU_RISCV32 is not None

# Dict mapping the cpu type to the correct qemu binary
_QEMU_BINARY_CPU_TYPE = dict[str, str](
    {
        'cortex-m0': _QEMU_ARM,
        'cortex-m3': _QEMU_ARM,
        'cortex-m33': _QEMU_ARM,
        'cortex-m4': _QEMU_ARM,
        'cortex-m55': _QEMU_ARM,
        'rv32': _QEMU_RISCV32,
        'rv32i': _QEMU_RISCV32,
        'rv32e': _QEMU_RISCV32,
    }
)


def _parse_args():
    """Parse and return command line arguments."""

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        '--machine',
        type=str,
        help='qemu machine type',
    )
    parser.add_argument(
        '--cpu',
        type=str,
        help='qemu cpu type',
    )
    parser.add_argument(
        '--image',
        type=str,
        help='image file to run',
    )
    parser.add_argument(
        '--semihosting',
        action='store_true',
        help='image file to run',
    )
    parser.add_argument(
        '--qemu-args',
        nargs='*',
        help='Extra arguments to pass to qemu',
    )
    parser.add_argument(
        '--uart-tcp-port',
        type=int,
        help='Expose serial port over local TCP socket on this port',
    )
    return parser.parse_known_args()


def _detokenizer(
    image: Path, tokenized_file: Path, qemu_finished: threading.Event
):
    try:
        detokenizer = detokenize.Detokenizer(image)
        line_buffer = ""
        with open(tokenized_file, 'r', buffering=1) as f:
            while not qemu_finished.is_set():
                try:
                    chunk = f.readline()
                    if chunk:
                        # qemu may not write a complete line, so buffer
                        # the chunks until there is a complete line to
                        # pass to the detokenizer.
                        line_buffer += chunk

                        # Use a while loop, as there could also potentially
                        # be multiple lines printed in-between iterations.
                        while '\n' in line_buffer:
                            newline_pos = line_buffer.find('\n') + 1
                            complete_line = line_buffer[:newline_pos]
                            detokenizer.detokenize_text_to_file(
                                complete_line, sys.stdout.buffer
                            )
                            sys.stdout.flush()

                            line_buffer = line_buffer[newline_pos:]
                except BlockingIOError:
                    # If writing to stdout too fast, it's sometimes possible
                    # to get BlockingIOError due to the stdout buffer being
                    # full, so sleep and try again.
                    time.sleep(0.1)

            # detokenize any remaining data in the buffer.
            if line_buffer:
                detokenizer.detokenize_text_to_file(
                    complete_line, sys.stdout.buffer
                )
                sys.stdout.flush()
    except OSError as e:
        print(f"Exception opening file {e}", file=sys.stderr)


def _main(args) -> int:
    try:
        qemu_exe: str = _QEMU_BINARY_CPU_TYPE[args.cpu.lower()]
    except KeyError:
        _LOG.fatal("unknown cpu type: %s", args.cpu)

    qemu_args = [
        qemu_exe,
        "-machine",
        args.machine,
        "-cpu",
        args.cpu,
        "-bios",
        "none",
        "-nographic",
        "-serial",
        (
            f"tcp:127.0.0.1:{args.uart_tcp_port},server,wait"
            if args.uart_tcp_port
            else "mon:stdio"
        ),
        "-kernel",
        args.image,
    ]

    if args.semihosting:
        qemu_args += [
            "-semihosting-config",
            "enable=on,target=native",
        ]

    if args.qemu_args:
        qemu_args.extend(args.qemu_args)

    _LOG.info("Invoking QEMU: %s", qemu_args)
    with tempfile.NamedTemporaryFile() as f:
        with subprocess.Popen(
            args=qemu_args,
            stdout=f,
        ) as proc:
            # Capturing the sub process stdout or stderr and then writing to
            # stdout can cause deadlocks (see
            # https://docs.python.org/3/library/subprocess.html#subprocess.Popen.stderr)
            # due to a write buffer (child process) filling up the pipe
            # buffer before the parent process can consume it.
            # To work around this, write to a temp file, and have the
            # detokenizer poll and detokenize the temp file.
            qemu_finished_event = threading.Event()
            stdout_thread = threading.Thread(
                target=_detokenizer,
                args=(Path(args.image), Path(f.name), qemu_finished_event),
                daemon=True,
            )
            stdout_thread.start()
            out, err = proc.communicate()
            qemu_finished_event.set()
            if out:
                print(out)
            if err:
                print(err)

            return_code = proc.returncode
    stdout_thread.join()

    sys.exit(return_code)


if __name__ == '__main__':
    known_args, remaining_args = _parse_args()
    if os.environ.get("PW_RUNNER_PASSTHROUGH") == "1":
        _LOG.info("Bypassing QEMU: %s", known_args.image)
        res = subprocess.run([known_args.image] + remaining_args)
        sys.exit(res.returncode)

    _main(known_args)
