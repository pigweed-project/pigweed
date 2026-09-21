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
"""Dump HDLC frames using pw_hdlc."""

import argparse
import signal
import sys
from collections import defaultdict
from pathlib import Path
from typing import BinaryIO, Iterable

import pw_cli.color
from pw_hdlc.decode import Frame, FrameDecoder

_COLOR = pw_cli.color.colors()


def _hexdump(data: bytes, prefix: str = "    ") -> str:
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off : off + 16]
        hex_bytes = " ".join(f"{b:02x}" for b in chunk)
        ascii_str = "".join(
            chr(b) if chr(b).isprintable() else "." for b in chunk
        )
        lines.append(f"{prefix}{off:04x}  {hex_bytes:<48}  |{ascii_str}|")
    return "\n".join(lines)


def _limited_hex(data: bytes, limit: int = 32, prefix: str = "") -> str:
    return f"{prefix}{data[:limit].hex()}{'…' if len(data) > limit else ''}"


def _preview(data: bytes, limit: int = 32) -> str:
    try:
        text = data.decode("utf-8").strip()
        if text.isprintable():
            return _COLOR.green(repr(text))
    except UnicodeDecodeError:
        pass
    return _COLOR.blue(_limited_hex(data, limit))


def _decode_stream(decoder: FrameDecoder, file: BinaryIO) -> Iterable[Frame]:
    while chunk := file.read(4096):
        yield from decoder.process(chunk)


def _dump_frames(
    infile: BinaryIO,
    output_dir: Path | None = None,
    show_hex: bool = False,
) -> None:
    ok_count = 0
    err_count = 0
    streams_by_addr: dict[int, bytearray] = defaultdict(bytearray)

    if output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)

    decoder = FrameDecoder()
    frames = _decode_stream(decoder, infile)
    for idx, frame in enumerate(frames):
        # Handle invalid frames
        if not frame.ok():
            err_count += 1
            print(
                _COLOR.red(
                    f"[{idx:04d}] ERROR[{frame.status.value}]"
                    f" raw_len={len(frame.raw_encoded)}"
                    f" raw={_limited_hex(frame.raw_encoded, 32)}"
                )
            )
            if show_hex:
                print(
                    _COLOR.gray(_hexdump(data=frame.raw_encoded, prefix="    "))
                )
                print()
            continue

        # Handle valid frames
        ok_count += 1
        streams_by_addr[frame.address].extend(frame.data)

        print(
            f"[{idx:04d}] OK addr=0x{frame.address:02x}"
            f" ctrl=0x{frame.control.hex()} len={len(frame.data):<3}"
            f" {_preview(frame.data)}"
        )
        if show_hex and len(frame.data) > 0:
            print(_COLOR.gray(_hexdump(frame.data)))
            print()

        if output_dir:
            by_addr_dir = output_dir / f"addr_{frame.address}"
            by_addr_dir.mkdir(exist_ok=True)

            frame_file = by_addr_dir / f"{idx:04d}.bin"
            with frame_file.open("wb") as out:
                out.write(frame.data)

    print(f"\nSummary: {ok_count} OK frames, {err_count} errors")

    # If extracting streams, write combined per-address binaries
    if output_dir:
        for addr, stream_data in streams_by_addr.items():
            combined_file = output_dir / f"stream_addr_{addr}.bin"
            with combined_file.open("wb") as out:
                out.write(stream_data)
            print(
                f"- Wrote combined stream for addr {addr}"
                f" ({len(stream_data)} bytes) -> {combined_file}"
            )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Dump HDLC frames from raw binary input using pw_hdlc.",
    )

    parser.add_argument(
        "infile",
        nargs="?",
        type=argparse.FileType("rb"),
        default=sys.stdin.buffer,
        help="Input file path (defaults to stdin if omitted)",
    )

    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        help="Directory to dump individual & combined payload binaries",
    )
    parser.add_argument(
        "--hex",
        action=argparse.BooleanOptionalAction,
        help="Print hex dump for every frame",
    )

    return parser.parse_args()


def _ignore_sigpipe() -> None:
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass


def main():
    args = _parse_args()

    # Ignore SIGPIPE when piped to head/less
    _ignore_sigpipe()

    _dump_frames(
        infile=args.infile,
        output_dir=args.output_dir,
        show_hex=args.hex,
    )


if __name__ == "__main__":
    main()
