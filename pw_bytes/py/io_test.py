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
"""Tests for pw_bytes.io"""
import io
import os
import unittest
import unittest.mock

from pw_bytes.io import BinaryReader, BinaryWriter


class BinaryReaderTest(unittest.TestCase):
    """BinaryReader tests."""

    def test_tell(self) -> None:
        f = io.BytesIO(b"12345")
        br = BinaryReader(f, "little")
        self.assertEqual(br.tell(), 0)

        f.seek(3)
        self.assertEqual(br.tell(), 3)

    def test_seek(self) -> None:
        f = io.BytesIO(b"12345")
        br = BinaryReader(f, "little")

        # 1. Verify standard absolute seek return value
        self.assertEqual(br.seek(3), 3)
        self.assertEqual(br.tell(), 3)

        # 2. Verify relative seek using SEEK_CUR
        self.assertEqual(br.seek(-1, os.SEEK_CUR), 2)
        self.assertEqual(br.tell(), 2)

        # 3. Verify relative seek using SEEK_END
        self.assertEqual(br.seek(-1, os.SEEK_END), 4)
        self.assertEqual(br.tell(), 4)

        # 4. Verify returning to start using f.seek directly
        f.seek(0)
        self.assertEqual(br.tell(), 0)

    def test_read(self) -> None:
        f = io.BytesIO(b"12345")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read(2), b"12")
        self.assertEqual(br.read(3), b"345")

    def test_read_then_tell(self) -> None:
        f = io.BytesIO(b"12345")
        br = BinaryReader(f, "little")
        self.assertEqual(br.tell(), 0)
        br.read(2)
        self.assertEqual(br.tell(), 2)

    def test_read_uint(self) -> None:
        f = io.BytesIO(b"\x11\x22\x33")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_uint(3), 0x332211)

    def test_read_uint_big(self) -> None:
        f = io.BytesIO(b"\x11\x22\x33\x44")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_uint(3, "big"), 0x112233)

    def test_read_u8(self) -> None:
        f = io.BytesIO(b"\x11")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_u8(), 0x11)

    def test_read_u16(self) -> None:
        f = io.BytesIO(b"\x11\x22")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_u16(), 0x2211)

    def test_read_u32(self) -> None:
        f = io.BytesIO(b"\x11\x22\x33\x44")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_u32(), 0x44332211)

    def test_read_u64(self) -> None:
        f = io.BytesIO(b"\x11\x22\x33\x44\x55\x66\x77\x88")
        br = BinaryReader(f, "little")
        self.assertEqual(br.read_u64(), 0x8877665544332211)


class BinaryWriterTest(unittest.TestCase):
    """BinaryWriter tests."""

    def test_tell(self) -> None:
        f = io.BytesIO(b"12345")
        bw = BinaryWriter(f, "little")
        self.assertEqual(bw.tell(), 0)

        f.seek(3)
        self.assertEqual(bw.tell(), 3)

    def test_seek(self) -> None:
        f = io.BytesIO(b"12345")
        bw = BinaryWriter(f, "little")

        # 1. Verify standard absolute seek return value
        self.assertEqual(bw.seek(3), 3)
        self.assertEqual(bw.tell(), 3)

        # 2. Verify relative seek using SEEK_CUR
        self.assertEqual(bw.seek(-1, os.SEEK_CUR), 2)
        self.assertEqual(bw.tell(), 2)

        # 3. Verify relative seek using SEEK_END
        self.assertEqual(bw.seek(-1, os.SEEK_END), 4)
        self.assertEqual(bw.tell(), 4)

        # 4. Verify returning to start using f.seek directly
        f.seek(0)
        self.assertEqual(bw.tell(), 0)

    def test_write(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write(b"hello")
        self.assertEqual(f.getvalue(), b"hello")

    def test_write_then_tell(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        self.assertEqual(bw.tell(), 0)
        bw.write(b"hello")
        self.assertEqual(bw.tell(), 5)

    def test_write_uint(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write_uint(0x332211, 3)
        bw.write_uint(0x44, 1)
        bw.write_uint(0x5566778899, 5, "big")
        self.assertEqual(f.getvalue(), b"\x11\x22\x33\x44\x55\x66\x77\x88\x99")

    def test_write_u8(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write_u8(0x11)
        self.assertEqual(f.getvalue(), b"\x11")

    def test_write_u16(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write_u16(0x2211)
        self.assertEqual(f.getvalue(), b"\x11\x22")

    def test_write_u32(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write_u32(0x44332211)
        self.assertEqual(f.getvalue(), b"\x11\x22\x33\x44")

    def test_write_u64(self) -> None:
        f = io.BytesIO()
        bw = BinaryWriter(f, "little")
        bw.write_u64(0x8877665544332211)
        self.assertEqual(f.getvalue(), b"\x11\x22\x33\x44\x55\x66\x77\x88")


class BinaryReaderRobustTest(unittest.TestCase):
    """Tests for BinaryReader robust behavior."""

    def test_read_eof(self) -> None:
        f = io.BytesIO(b"12")
        br = BinaryReader(f, "little")
        with self.assertRaises(EOFError) as ctx:
            br.read(3)
        self.assertIn("Reached EOF: read 2 of 3 bytes", str(ctx.exception))

    def test_read_blocking(self) -> None:
        reader = unittest.mock.Mock()
        reader.read.return_value = None

        br = BinaryReader(reader, "little")
        with self.assertRaises(BlockingIOError):
            br.read(3)

    def test_read_partial(self) -> None:
        reader = unittest.mock.Mock()
        reader.read.side_effect = [b"1", b"2", b"3"]

        br = BinaryReader(reader, "little")
        self.assertEqual(br.read(3), b"123")


class BinaryWriterRobustTest(unittest.TestCase):
    """Tests for BinaryWriter robust behavior."""

    def test_write_blocking(self) -> None:
        writer = unittest.mock.Mock()
        writer.write.return_value = None

        bw = BinaryWriter(writer, "little")
        with self.assertRaises(BlockingIOError):
            bw.write(b"123")

    def test_write_zero_bytes(self) -> None:
        writer = unittest.mock.Mock()
        writer.write.return_value = 0

        bw = BinaryWriter(writer, "little")
        with self.assertRaises(OSError) as ctx:
            bw.write(b"123")
        self.assertIn("Zero bytes written", str(ctx.exception))

    def test_write_partial(self) -> None:
        writer = unittest.mock.Mock()
        writer.write.side_effect = lambda data: len(data[:1])

        bw = BinaryWriter(writer, "little")
        bw.write(b"123")
        self.assertEqual(
            writer.write.call_args_list,
            [
                unittest.mock.call(b"123"),
                unittest.mock.call(b"23"),
                unittest.mock.call(b"3"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
