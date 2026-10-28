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
"""Tests for raw COBS encoding and decoding (parity with cobs_test.cc)."""

import random
import unittest

from pw_cobs import cobs


def _nonzero(length: int, modulus: int = 255) -> bytes:
    return bytes((i % modulus) + 1 for i in range(length))


def _reference_decode(encoded: bytes) -> bytes:
    """Straightforward COBS decoder used to cross-check cobs.decode()."""
    body = encoded[:-1]
    out = bytearray()
    i = 0
    while i < len(body):
        code = body[i]
        out += body[i + 1 : i + code]
        i += code
        if code != 0xFF and i < len(body):
            out.append(0)
    return bytes(out)


class KnownVectorsTest(unittest.TestCase):
    """Golden vectors from pw_cobs/cpp/cobs_test.cc and the COBS paper."""

    VECTORS = [
        (b'', b'\x01\x00'),
        (b'\x00', b'\x01\x01\x00'),
        (b'\x00\x00', b'\x01\x01\x01\x00'),
        (b'\x11\x22\x00\x33', b'\x03\x11\x22\x02\x33\x00'),
        (b'\x11\x22\x33\x44', b'\x05\x11\x22\x33\x44\x00'),
        (b'\x11\x00\x00\x00', b'\x02\x11\x01\x01\x01\x00'),
        (b'\x00\x11\x00', b'\x01\x02\x11\x01\x00'),
    ]

    def test_encode(self) -> None:
        for raw, encoded in self.VECTORS:
            with self.subTest(raw=raw):
                self.assertEqual(cobs.encode(raw), encoded)

    def test_decode(self) -> None:
        for raw, encoded in self.VECTORS:
            with self.subTest(raw=raw):
                self.assertEqual(cobs.decode(encoded), raw)

    def test_accepts_bytearray_and_memoryview(self) -> None:
        self.assertEqual(
            cobs.encode(bytearray(b'\x11\x00')), cobs.encode(b'\x11\x00')
        )
        self.assertEqual(
            cobs.decode(memoryview(b'\x02\x11\x01\x00')), b'\x11\x00'
        )


class BoundaryRunLengthTest(unittest.TestCase):
    """Mirrors Cobs.BoundaryRunLengths253To510."""

    def test_boundary_lengths(self) -> None:
        for length in (253, 254, 255, 256, 507, 508, 509, 510):
            with self.subTest(length=length):
                data = _nonzero(length)
                encoded = cobs.encode(data)
                self.assertLessEqual(
                    len(encoded), cobs.max_encoded_size(length)
                )
                self.assertNotIn(0, encoded[:-1])
                self.assertEqual(encoded[-1], 0x00)
                if length == 253:
                    self.assertEqual(len(encoded), 255)
                    self.assertEqual(encoded[0], 0xFE)
                elif length == 254:
                    self.assertEqual(len(encoded), 256)
                    self.assertEqual(encoded[0], 0xFF)
                elif length == 255:
                    self.assertEqual(len(encoded), 258)
                    self.assertEqual(encoded[0], 0xFF)
                    self.assertEqual(encoded[255], 0x02)
                elif length == 508:
                    self.assertEqual(len(encoded), 511)
                    self.assertEqual(encoded[0], 0xFF)
                    self.assertEqual(encoded[255], 0xFF)
                self.assertEqual(cobs.decode(encoded), data)

                with_zero = data + b'\x00'
                encoded_z = cobs.encode(with_zero)
                self.assertEqual(cobs.decode(encoded_z), with_zero)

    def test_254_nonzero_then_zero(self) -> None:
        data = _nonzero(254) + b'\x00'
        self.assertEqual(
            cobs.encode(data), b'\xff' + _nonzero(254) + b'\x01\x01\x00'
        )

    def test_254_nonzero_exactly_has_no_trailing_code(self) -> None:
        self.assertEqual(
            cobs.encode(_nonzero(254)), b'\xff' + _nonzero(254) + b'\x00'
        )

    def test_decode_accepts_trailing_empty_block_after_max_run(self) -> None:
        # Not canonical, but accepted by pw::cobs::Decode.
        self.assertEqual(
            cobs.decode(b'\xff' + _nonzero(254) + b'\x01\x00'), _nonzero(254)
        )


class MaxEncodedSizeTest(unittest.TestCase):
    """Tests for the encoded and decoded size helpers."""

    def test_values(self) -> None:
        self.assertEqual(cobs.max_encoded_size(0), 2)
        self.assertEqual(cobs.max_encoded_size(1), 3)
        self.assertEqual(cobs.max_encoded_size(254), 256)
        self.assertEqual(cobs.max_encoded_size(255), 258)
        self.assertEqual(cobs.max_decoded_size(0), 0)
        self.assertEqual(cobs.max_decoded_size(1), 0)
        self.assertEqual(cobs.max_decoded_size(2), 0)
        self.assertEqual(cobs.max_decoded_size(3), 1)
        self.assertEqual(cobs.max_decoded_size(256), 254)

    def test_negative(self) -> None:
        with self.assertRaises(ValueError):
            cobs.max_encoded_size(-1)
        with self.assertRaises(ValueError):
            cobs.max_decoded_size(-1)

    def test_bound_holds(self) -> None:
        rng = random.Random(1)
        for length in range(0, 1100, 7):
            for data in (
                bytes(length),
                _nonzero(length),
                bytes(rng.getrandbits(8) for _ in range(length)),
            ):
                self.assertLessEqual(
                    len(cobs.encode(data)), cobs.max_encoded_size(length)
                )


class DecodeErrorTest(unittest.TestCase):
    """Mirrors Cobs.DecodeDetectsMalformedFrames."""

    def test_empty(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'')

    def test_bare_delimiter(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x00')

    def test_missing_delimiter(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x01')
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x02\x11')

    def test_embedded_zero(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x03\x11\x00\x00')

    def test_zero_code_byte(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x02\x11\x00\x01\x00')

    def test_truncated_run(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\x05\x11\x22\x00')

    def test_truncated_max_run(self) -> None:
        with self.assertRaises(cobs.CobsDecodeError):
            cobs.decode(b'\xff' + _nonzero(253) + b'\x00')

    def test_is_value_error(self) -> None:
        self.assertTrue(issubclass(cobs.CobsDecodeError, ValueError))


class RoundTripTest(unittest.TestCase):
    """Randomized encode/decode round trips."""

    def test_random(self) -> None:
        rng = random.Random(0x5EED)
        for _ in range(500):
            length = rng.randrange(0, 1200)
            zero_prob = rng.choice([0.0, 0.01, 0.1, 0.5, 1.0])
            data = bytes(
                0 if rng.random() < zero_prob else rng.randrange(1, 256)
                for _ in range(length)
            )
            encoded = cobs.encode(data)
            self.assertNotIn(0, encoded[:-1])
            self.assertEqual(encoded[-1], 0x00)
            self.assertEqual(_reference_decode(encoded), data)
            self.assertEqual(cobs.decode(encoded), data)


def _concat_streamer(data: bytes) -> bytes:
    streamer = cobs.Streamer()
    rem = data
    out = bytearray()
    while rem:
        rem = streamer.write(rem)
        if rem:
            out += streamer.flush()
    out += streamer.finish()
    return bytes(out)


class StreamerTest(unittest.TestCase):
    """Tests for Streamer, which mirrors pw::cobs::Streamer."""

    def test_matches_encode(self) -> None:
        rng = random.Random(7)
        cases = [b'', b'\x00', b'\x00\x00', _nonzero(254), _nonzero(255)]
        cases.append(_nonzero(254) + b'\x00')
        cases += [
            bytes(rng.choice([0, 1, 0x55, 0xFF]) for _ in range(n))
            for n in range(0, 600, 13)
        ]
        for data in cases:
            with self.subTest(data=data[:8], length=len(data)):
                self.assertEqual(_concat_streamer(data), cobs.encode(data))

    def test_segments_prefix_body_suffix(self) -> None:
        prefix = b'\x38\x00\x01\x00'
        suffix = b'\xde\xad\xbe\xef'
        body = bytes(0 if i in (12, 50) else (i % 250) + 1 for i in range(400))
        streamer = cobs.Streamer()
        self.assertEqual(streamer.abort_sequence(), b'')
        out = bytearray()
        for seg in (prefix, body, suffix):
            rem = seg
            while rem:
                rem = streamer.write(rem)
                if rem:
                    flushed = streamer.flush()
                    self.assertTrue(flushed)
                    out += flushed
        self.assertEqual(streamer.abort_sequence(), b'\xff\x00')
        out += streamer.finish()
        self.assertEqual(streamer.abort_sequence(), b'')
        self.assertEqual(bytes(out), cobs.encode(prefix + body + suffix))

    def test_abort_sequence_after_flush(self) -> None:
        streamer = cobs.Streamer()
        rem = streamer.write(_nonzero(300))
        self.assertTrue(rem)
        first = streamer.flush()
        self.assertTrue(first)
        self.assertEqual(streamer.abort_sequence(), b'\xff\x00')
        streamer.reset()
        self.assertEqual(streamer.abort_sequence(), b'')

    def test_leading_delimiter(self) -> None:
        streamer = cobs.Streamer(leading_delimiter=True)
        self.assertEqual(streamer.write(b'hello'), b'')
        self.assertEqual(streamer.finish(), b'\x00' + cobs.encode(b'hello'))
        streamer.reset(leading_delimiter=True)
        self.assertEqual(streamer.write(b'world'), b'')
        self.assertEqual(streamer.finish(), b'\x00' + cobs.encode(b'world'))


class StreamingDecoderTest(unittest.TestCase):
    """Tests for cobs.Decoder, which mirrors pw::cobs::Decoder."""

    def test_decodes_frames_and_recovers_from_errors(self) -> None:
        decoder = cobs.Decoder(buffer_size=32)
        wire = (
            b'\x00'
            + cobs.encode(b'hello')
            + b'\x05\x11\x22\x00'
            + cobs.encode(b'world')
        )
        frames = list(decoder.process(wire))
        self.assertEqual(len(frames), 3)
        self.assertTrue(frames[0].ok())
        self.assertEqual(frames[0].data, b'hello')
        self.assertIs(frames[1].status, cobs.DecodeStatus.DATA_LOSS)
        self.assertTrue(frames[2].ok())
        self.assertEqual(frames[2].data, b'world')


if __name__ == '__main__':
    unittest.main()
