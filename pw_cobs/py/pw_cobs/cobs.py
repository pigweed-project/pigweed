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
"""Consistent Overhead Byte Stuffing (COBS) encoding, streaming, and decoding.

This module is byte-for-byte compatible with ``pw_cobs/cobs.h``.
"""

from __future__ import annotations

from dataclasses import dataclass
import enum
from typing import Iterator

#: Maximum number of contiguous non-zero bytes in a single COBS block.
MAX_RUN_LENGTH = 254

#: Minimum buffer size required for streaming a full 254-byte COBS block plus
#: its code byte and a leading or trailing ``0x00`` delimiter.
MIN_BUFFER_SIZE = MAX_RUN_LENGTH + 3

#: 2-byte abort sequence (``b'\\xff\\x00'``) emitted at a COBS block boundary to
#: invalidate a partially transmitted frame.
ABORT_SEQUENCE = b'\xff\x00'

_FRAME_DELIMITER = 0x00
_MAX_RUN_CODE = 0xFF


class CobsDecodeError(ValueError):
    """Raised when a COBS-encoded frame is malformed or truncated.

    Corresponds to ``DATA_LOSS`` from ``pw::cobs::Decode``.
    """


def max_encoded_size(unencoded_size: int) -> int:
    """Worst-case COBS-encoded frame size, including the trailing ``0x00``.

    Matches ``pw::cobs::MaxEncodedSize``.
    """
    if unencoded_size < 0:
        raise ValueError('unencoded_size must be non-negative')
    if unencoded_size == 0:
        return 2
    return (
        unencoded_size
        + (unencoded_size + MAX_RUN_LENGTH - 1) // MAX_RUN_LENGTH
        + 1
    )


def max_decoded_size(encoded_size: int) -> int:
    """Maximum decoded payload size for a COBS frame of ``encoded_size`` bytes.

    Matches ``pw::cobs::MaxDecodedSize``.
    """
    if encoded_size < 0:
        raise ValueError('encoded_size must be non-negative')
    return max(0, encoded_size - 2)


@dataclass(frozen=True)
class Chunk:
    """A wire chunk produced by streaming a COBS frame."""

    #: Wire bytes to transmit.
    data: bytes

    #: True if this is the final chunk of the frame (ends with ``0x00``).
    frame_complete: bool = False


class Streamer:
    """Incrementally COBS-encodes a frame into complete COBS blocks.

    Mirrors ``pw::cobs::Streamer`` using a fixed internal buffer (defaulting to
    ``MIN_BUFFER_SIZE`` = 257 bytes) where ``flush()`` only emits completed
    COBS blocks and never leaves an incomplete block in the tail.
    """

    MIN_BUFFER_SIZE = MIN_BUFFER_SIZE
    ABORT_SEQUENCE = ABORT_SEQUENCE

    def __init__(
        self,
        buffer_size: int = MIN_BUFFER_SIZE,
        *,
        leading_delimiter: bool = False,
    ) -> None:
        if buffer_size < 2:
            raise ValueError('buffer_size must be at least 2')
        self._buffer = bytearray(buffer_size)
        self._lead_offset = 0
        self._code_pos = 0
        self._write_pos = 1
        self._code = 1
        self._flushed_any = False
        self.reset(leading_delimiter=leading_delimiter)

    def reset(self, *, leading_delimiter: bool = False) -> None:
        """Resets the streamer to begin a new frame."""
        self._flushed_any = False
        if leading_delimiter:
            self._buffer[0] = _FRAME_DELIMITER
            self._lead_offset = 1
            self._code_pos = 1
            self._write_pos = 2
        else:
            self._lead_offset = 0
            self._code_pos = 0
            self._write_pos = 1
        self._code = 1

    def abort_sequence(self) -> bytes:
        """Returns ``b'\\xff\\x00'`` if at least one block was flushed."""
        return self.ABORT_SEQUENCE if self._flushed_any else b''

    def write(self, data: bytes | bytearray | memoryview) -> bytes:
        """Encodes as much of ``data`` as fits into the buffer.

        Returns the unconsumed remainder of ``data``.
        """
        view = memoryview(data)
        buf_size = len(self._buffer)
        while view:
            if self._code == _MAX_RUN_CODE:
                if self._write_pos + 1 >= buf_size:
                    break
                self._buffer[self._code_pos] = _MAX_RUN_CODE
                self._code_pos = self._write_pos
                self._write_pos += 1
                self._code = 1

            avail_out = (buf_size - 1) - self._write_pos
            if avail_out == 0:
                break

            max_run = min(len(view), avail_out, _MAX_RUN_CODE - self._code)
            chunk_bytes = bytes(view[:max_run])
            zero_pos = chunk_bytes.find(_FRAME_DELIMITER)
            non_zero = max_run if zero_pos == -1 else zero_pos
            hit_zero = (non_zero < max_run) or (
                non_zero < len(view)
                and (self._code + non_zero < _MAX_RUN_CODE)
                and view[non_zero] == _FRAME_DELIMITER
            )
            block_completes = hit_zero or (
                self._code + non_zero == _MAX_RUN_CODE
            )

            if (hit_zero and self._write_pos + non_zero + 1 >= buf_size) or (
                self._code_pos > self._lead_offset and not block_completes
            ):
                break

            if non_zero > 0:
                self._buffer[self._write_pos : self._write_pos + non_zero] = (
                    view[:non_zero]
                )
                self._write_pos += non_zero
                self._code += non_zero
                view = view[non_zero:]

            if hit_zero:
                self._buffer[self._code_pos] = self._code
                self._code_pos = self._write_pos
                self._write_pos += 1
                self._code = 1
                view = view[1:]

        return bytes(view)

    def flush(self) -> bytes:
        """Returns all completed COBS blocks currently in the buffer."""
        if self._code == _MAX_RUN_CODE:
            self._buffer[self._code_pos] = _MAX_RUN_CODE
            self._code_pos = self._write_pos
            self._code = 1
        if self._code_pos <= self._lead_offset:
            return b''
        flushed = bytes(self._buffer[: self._code_pos])
        self._lead_offset = 0
        self._code_pos = 0
        self._write_pos = 1
        self._flushed_any = True
        return flushed

    def finish(self) -> bytes:
        """Finalizes the active block, appends ``0x00``, and resets."""
        self._buffer[self._code_pos] = self._code
        self._buffer[self._write_pos] = _FRAME_DELIMITER
        self._write_pos += 1
        final_chunk = bytes(self._buffer[: self._write_pos])
        self.reset()
        return final_chunk


def encode(data: bytes | bytearray | memoryview) -> bytes:
    """COBS-encodes ``data`` into a ``0x00``-terminated frame.

    Matches ``pw::cobs::Encode``. An empty input encodes as ``b'\\x01\\x00'``.
    """
    rem = bytes(data)
    streamer = Streamer(buffer_size=max_encoded_size(len(rem)))
    out = bytearray()
    while rem:
        rem = streamer.write(rem)
        if rem:
            out += streamer.flush()
    out += streamer.finish()
    return bytes(out)


class DecodeStatus(enum.Enum):
    """Result status of :class:`Decoder`."""

    OK = 'OK'
    DATA_LOSS = 'DATA_LOSS'
    RESOURCE_EXHAUSTED = 'RESOURCE_EXHAUSTED'


@dataclass(frozen=True)
class DecodedFrame:
    """A COBS frame decoded by :class:`Decoder`."""

    status: DecodeStatus
    data: bytes = b''

    def ok(self) -> bool:
        """True if this frame decoded successfully."""
        return self.status is DecodeStatus.OK


class Decoder:
    """Incrementally decodes ``0x00``-delimited COBS frames from a byte stream.

    Mirrors ``pw::cobs::Decoder``.
    """

    def __init__(self, buffer_size: int | None = None) -> None:
        if buffer_size is not None and buffer_size < 0:
            raise ValueError('buffer_size must be non-negative')
        self._buffer_size = buffer_size
        self._decoded = bytearray()
        self._remaining_in_run = 0
        self._started = False
        self._insert_implicit_zero = False
        self._overflowed = False

    @property
    def buffer_size(self) -> int | None:
        """Maximum decoded frame size, or None if unlimited."""
        return self._buffer_size

    @property
    def in_progress(self) -> bool:
        """True if non-delimiter bytes have been received for a frame."""
        return self._started

    def clear(self) -> None:
        """Discards any partially received frame."""
        self._decoded = bytearray()
        self._remaining_in_run = 0
        self._started = False
        self._insert_implicit_zero = False
        self._overflowed = False

    def _append(self, data: bytes) -> None:
        if self._overflowed:
            return
        if (
            self._buffer_size is not None
            and len(self._decoded) + len(data) > self._buffer_size
        ):
            self._overflowed = True
            return
        self._decoded += data

    def _finalize(self) -> DecodedFrame | None:
        truncated = self._remaining_in_run > 0
        overflowed = self._overflowed
        started = self._started
        decoded = bytes(self._decoded)
        self.clear()

        if not started:
            return None
        if truncated:
            return DecodedFrame(DecodeStatus.DATA_LOSS)
        if overflowed:
            return DecodedFrame(DecodeStatus.RESOURCE_EXHAUSTED)
        return DecodedFrame(DecodeStatus.OK, decoded)

    def process(
        self, data: bytes | bytearray | memoryview
    ) -> Iterator[DecodedFrame]:
        """Decodes ``data``, yielding each completed or invalid COBS frame."""
        data = bytes(data)
        pos = 0
        end = len(data)
        while pos < end:
            if self._remaining_in_run > 0:
                step_end = pos + min(end - pos, self._remaining_in_run)
                zero = data.find(0, pos, step_end)
                if zero != -1:
                    pos = zero + 1
                    frame = self._finalize()
                    if frame is not None:
                        yield frame
                    continue
                run = data[pos:step_end]
                self._append(run)
                self._remaining_in_run -= len(run)
                pos = step_end
                continue

            code = data[pos]
            pos += 1
            if code == _FRAME_DELIMITER:
                frame = self._finalize()
                if frame is not None:
                    yield frame
                continue

            self._started = True
            if self._insert_implicit_zero:
                self._append(b'\x00')
            self._remaining_in_run = code - 1
            self._insert_implicit_zero = code != _MAX_RUN_CODE

    def process_byte(self, byte: int) -> DecodedFrame | None:
        """Decodes one byte; returns a frame if one completed on it."""
        for frame in self.process(bytes([byte])):
            return frame
        return None


def decode(encoded: bytes | bytearray | memoryview) -> bytes:
    """Decodes a ``0x00``-terminated COBS frame (``pw::cobs::Decode``).

    An empty frame ``b'\\x01\\x00'`` decodes to ``b''``.
    """
    raw = bytes(encoded)
    if len(raw) < 2 or raw[-1] != _FRAME_DELIMITER or 0 in raw[:-1]:
        raise CobsDecodeError(
            'COBS frame must end with a single 0x00 delimiter'
        )
    decoder = Decoder()
    frames = list(decoder.process(raw))
    if len(frames) != 1 or not frames[0].ok():
        raise CobsDecodeError('Malformed or truncated COBS frame')
    return frames[0].data
