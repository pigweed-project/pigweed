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
"""Classes for reading and writing binary data streams."""

import os
from typing import IO, Literal

ByteOrder = Literal["little", "big"]


class BinaryReader:
    """Facilitates reading binary data (integers) from an I/O stream."""

    def __init__(self, reader: IO[bytes], byteorder: ByteOrder):
        """Creates a new BinaryReader instance wrapping the given reader.

        Args:
          reader: The I/O stream from which to read.
            Note that BinaryReader does not "own" the stream; it "borrows" it.
          byteorder: The byte order of multi-byte integers; either "little"
            (least significant byte first) or "big" (most significant byte
            first).
        """
        if not reader.readable():
            raise ValueError("reader is not readable")
        self.reader = reader
        self.byteorder = byteorder

    def tell(self) -> int:
        """Returns the current position of the underlying stream.

        See io.IOBase.tell().
        """
        return self.reader.tell()

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        """Changes the position of the underlying stream.

        See io.IOBase.seek().
        """
        return self.reader.seek(offset, whence)

    def read(self, n: int) -> bytes:
        """Reads exactly n bytes from the underlying stream.

        Args:
            n: The exact number of bytes to read.

        Returns:
            The read binary bytes (guaranteed to be exactly n bytes long).

        Raises:
            EOFError: If EOF is reached before reading n bytes.
            BlockingIOError: If the stream is in non-blocking mode and
                no bytes are available to be read.
        """
        chunks = []
        bytes_left = n
        while bytes_left > 0:
            chunk = self.reader.read(bytes_left)
            # io.RawIOBase.read() says:
            # If 0 bytes are returned, and size was not 0, this indicates
            # end-of-file. If the object is in non-blocking mode and no bytes
            # are available, None is returned.
            if chunk is None:
                raise BlockingIOError("Read would block")
            if not chunk:
                raise EOFError(
                    f"Reached EOF: read {n - bytes_left} of {n} bytes"
                )
            chunks.append(chunk)
            bytes_left -= len(chunk)
        return b"".join(chunks)

    def read_uint(self, size: int, byteorder: ByteOrder | None = None) -> int:
        """Reads an unsigned integer of the given byte size from the stream.

        Args:
            size: The number of bytes representing the integer (e.g., 2, 4, 8).
            byteorder: The byte order of the integer ("little" or "big").
                If not specified, defaults to the reader's default byte order.

        Returns:
            The unpacked unsigned integer value.

        Raises:
            EOFError: If EOF is reached before reading size bytes.
            BlockingIOError: If the stream is in non-blocking mode and
                no bytes are available to be read.
        """
        data = self.read(size)
        return int.from_bytes(data, byteorder=byteorder or self.byteorder)

    def read_u8(self, byteorder: ByteOrder | None = None) -> int:
        """Reads an unsigned 8-bit integer from the stream."""
        return self.read_uint(size=1, byteorder=byteorder)

    def read_u16(self, byteorder: ByteOrder | None = None) -> int:
        """Reads an unsigned 16-bit integer from the stream."""
        return self.read_uint(size=2, byteorder=byteorder)

    def read_u32(self, byteorder: ByteOrder | None = None) -> int:
        """Reads an unsigned 32-bit integer from the stream."""
        return self.read_uint(size=4, byteorder=byteorder)

    def read_u64(self, byteorder: ByteOrder | None = None) -> int:
        """Reads an unsigned 64-bit integer from the stream."""
        return self.read_uint(size=8, byteorder=byteorder)


class BinaryWriter:
    """Facilitates writing binary data (integers) to a stream."""

    def __init__(self, writer: IO[bytes], byteorder: ByteOrder):
        """Creates a new BinaryWriter instance wrapping the given writer.

        Args:
          writer: The I/O stream to which to write.
            Note that BinaryWriter does not "own" the stream; it "borrows" it.
          byteorder: The byte order of multi-byte integers; either "little"
            (least significant byte first) or "big" (most significant byte
            first).
        """
        if not writer.writable():
            raise ValueError("writer is not writable")
        self.writer = writer
        self.byteorder = byteorder

    def tell(self) -> int:
        """Returns the current position of the underlying stream.

        See io.IOBase.tell().
        """
        return self.writer.tell()

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        """Changes the position of the underlying stream.

        See io.IOBase.seek().
        """
        return self.writer.seek(offset, whence)

    def write(self, data: bytes) -> None:
        """Writes all data to the underlying stream.

        Args:
            data: The raw binary bytes to write.

        Raises:
            OSError: If an I/O error occurs (e.g. zero bytes written).
            BlockingIOError: If the stream is in non-blocking mode and
                no bytes can be written to it.
        """
        offset = 0
        total = len(data)
        view = memoryview(data)
        while offset < total:
            written = self.writer.write(view[offset:])
            # io.RawIOBase.write() says:
            # None is returned if the raw stream is set not to block and no
            # single byte could be readily written to it.
            if written is None:
                raise BlockingIOError("Write would block")
            if written == 0:
                raise OSError("Zero bytes written")
            offset += written

    def write_uint(
        self, val: int, length: int, byteorder: ByteOrder | None = None
    ) -> None:
        """Writes an unsigned integer of the given byte length to the stream.

        Args:
            val: The unsigned integer value to pack and write.
            length: The size of the integer in bytes (e.g., 2, 4, 8).
            byteorder: The byte order of the integer ("little" or "big").
                If not specified, defaults to the writer's default byte order.

        Raises:
            OverflowError: If the integer value is negative or too large for the
                specified byte length.
            BlockingIOError: If the stream is in non-blocking mode and
                no bytes can be written to it.
        """
        data = val.to_bytes(
            length=length, byteorder=byteorder or self.byteorder
        )
        self.write(data)

    def write_u8(self, val: int, byteorder: ByteOrder | None = None) -> None:
        """Writes an unsigned 8-bit integer to the stream."""
        return self.write_uint(val=val, length=1, byteorder=byteorder)

    def write_u16(self, val: int, byteorder: ByteOrder | None = None) -> None:
        """Writes an unsigned 16-bit integer to the stream."""
        return self.write_uint(val=val, length=2, byteorder=byteorder)

    def write_u32(self, val: int, byteorder: ByteOrder | None = None) -> None:
        """Writes an unsigned 32-bit integer to the stream."""
        return self.write_uint(val=val, length=4, byteorder=byteorder)

    def write_u64(self, val: int, byteorder: ByteOrder | None = None) -> None:
        """Writes an unsigned 64-bit integer to the stream."""
        return self.write_uint(val=val, length=8, byteorder=byteorder)
