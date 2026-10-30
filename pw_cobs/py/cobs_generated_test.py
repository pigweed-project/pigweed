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
"""Data-driven COBS tests for Python and generated C++ unit tests."""

from __future__ import annotations

from typing import Iterator, NamedTuple, Sequence

from pw_build import generated_tests
from pw_build.generated_tests import (
    Context,
    GroupOrTest,
    PyTest,
    TestGenerator,
)
from pw_cobs import cobs
from pw_codegen import cpp


def _nonzero_255(count: int, start: int = 1) -> bytes:
    """Cycles through 0x01..0xFF (255 distinct non-zero byte values)."""
    return bytes(1 + (start - 1 + i) % 255 for i in range(count))


def _nonzero_254(count: int) -> bytes:
    """Cycles through 0x01..0xFE (254 distinct non-zero byte values)."""
    return bytes(1 + i % 254 for i in range(count))


def _counting(count: int) -> bytes:
    """Cycles through 0x00..0xFF."""
    return bytes(i & 0xFF for i in range(count))


def _mixed(count: int, seed: int = 0) -> bytes:
    """Deterministic full-period (gcd(13, 256) == 1) byte pattern."""
    return bytes((seed * 7 + i * 13) & 0xFF for i in range(count))


_NONZERO_253 = _nonzero_255(253)
_NONZERO_254 = _nonzero_255(254)
_NONZERO_255 = _nonzero_255(255)
_NONZERO_508 = _nonzero_255(508)
_NONZERO_509 = _nonzero_255(509)
_MIXED_PERIOD = _mixed(256)
_MIXED_1000 = _mixed(1000)


class EncodeCase(NamedTuple):
    decoded: bytes
    encoded: bytes


class DecodeCase(NamedTuple):
    encoded: bytes
    decoded: bytes | None  # None indicates Status::DataLoss / CobsDecodeError


class ExpectedDecoderFrame(NamedTuple):
    status: cobs.DecodeStatus
    decoded: bytes = b''

    @classmethod
    def ok(cls, decoded: bytes) -> ExpectedDecoderFrame:
        return cls(cobs.DecodeStatus.OK, decoded)

    @classmethod
    def error(cls, status: cobs.DecodeStatus) -> ExpectedDecoderFrame:
        return cls(status, b'')


class DecoderCase(NamedTuple):
    buffer_size: int
    wire: bytes
    expected: Sequence[ExpectedDecoderFrame]


ENCODE_TEST_CASES: tuple[GroupOrTest[EncodeCase], ...] = (
    'Empty',
    EncodeCase(b'', b'\x01\x00'),
    'Single zero',
    EncodeCase(b'\x00', b'\x01\x01\x00'),
    'Two zeros',
    EncodeCase(b'\x00\x00', b'\x01\x01\x01\x00'),
    'Zero run 8',
    EncodeCase(b'\x00' * 8, b'\x01' * 9 + b'\x00'),
    'Zero in middle',
    EncodeCase(b'\x11\x22\x00\x33', b'\x03\x11\x22\x02\x33\x00'),
    'No zeros',
    EncodeCase(b'\x11\x22\x33\x44', b'\x05\x11\x22\x33\x44\x00'),
    'Trailing zeros',
    EncodeCase(b'\x11\x00\x00\x00', b'\x02\x11\x01\x01\x01\x00'),
    'Leading zero',
    EncodeCase(b'\x00\x11', b'\x01\x02\x11\x00'),
    'Single ff',
    EncodeCase(b'\xff', b'\x02\xff\x00'),
    'Run 253',
    EncodeCase(_NONZERO_253, b'\xfe' + _NONZERO_253 + b'\x00'),
    'Run 254',
    EncodeCase(_NONZERO_254, b'\xff' + _NONZERO_254 + b'\x00'),
    'Run 255',
    EncodeCase(_NONZERO_255, b'\xff' + _NONZERO_254 + b'\x02\xff\x00'),
    'Zero then run 254',
    EncodeCase(
        b'\x00' + _NONZERO_254,
        b'\x01\xff' + _NONZERO_254 + b'\x00',
    ),
    'Run 254 then zero',
    EncodeCase(
        _NONZERO_254 + b'\x00',
        b'\xff' + _NONZERO_254 + b'\x01\x01\x00',
    ),
    'Run 254 then zero then byte',
    EncodeCase(
        _NONZERO_254 + b'\x00\x01',
        b'\xff' + _NONZERO_254 + b'\x01\x02\x01\x00',
    ),
    'Run 253 then zero',
    EncodeCase(
        _NONZERO_253 + b'\x00',
        b'\xfe' + _NONZERO_253 + b'\x01\x00',
    ),
    'Ff 254 then zero',
    EncodeCase(
        b'\xff' * 254 + b'\x00',
        b'\xff' * 255 + b'\x01\x01\x00',
    ),
    'Wikipedia 00 to fe',
    EncodeCase(
        bytes(range(0x00, 0xFF)),
        b'\x01\xff' + bytes(range(0x01, 0xFF)) + b'\x00',
    ),
    'Wikipedia 01 to ff',
    EncodeCase(
        bytes(range(0x01, 0x100)),
        b'\xff' + bytes(range(0x01, 0xFF)) + b'\x02\xff\x00',
    ),
    'Wikipedia 02 to ff 00',
    EncodeCase(
        bytes(range(0x02, 0x100)) + b'\x00',
        b'\xff' + bytes(range(0x02, 0x100)) + b'\x01\x01\x00',
    ),
    'Wikipedia 03 to ff 00 01',
    EncodeCase(
        bytes(range(0x03, 0x100)) + b'\x00\x01',
        b'\xfe' + bytes(range(0x03, 0x100)) + b'\x02\x01\x00',
    ),
    'Run 508',
    EncodeCase(
        _NONZERO_508,
        b'\xff' + _NONZERO_508[:254] + b'\xff' + _NONZERO_508[254:] + b'\x00',
    ),
    'Run 509',
    EncodeCase(
        _NONZERO_509,
        b'\xff'
        + _NONZERO_509[:254]
        + b'\xff'
        + _NONZERO_509[254:508]
        + b'\x02\xfe\x00',
    ),
    'Counting 300',
    EncodeCase(
        _counting(300),
        b'\x01\xff'
        + bytes(range(1, 255))
        + b'\x02\xff\x2c'
        + bytes(range(1, 44))
        + b'\x00',
    ),
    'Zeros 300',
    EncodeCase(b'\x00' * 300, b'\x01' * 301 + b'\x00'),
    'Mixed 1000',
    EncodeCase(
        _MIXED_1000,
        b'\x01'
        + (b'\xff' + _MIXED_PERIOD[1:255] + b'\x02\xf3') * 3
        + b'\xe8'
        + _MIXED_PERIOD[1:232]
        + b'\x00',
    ),
    'Alternating zero',
    EncodeCase(b'\x00\x01' * 100, b'\x01' + b'\x02\x01' * 100 + b'\x00'),
)

DECODE_TEST_CASES: tuple[GroupOrTest[DecodeCase], ...] = (
    'Empty block',
    DecodeCase(b'\x01\x00', b''),
    'Single zero',
    DecodeCase(b'\x01\x01\x00', b'\x00'),
    'Zero in middle',
    DecodeCase(b'\x03\x11\x22\x02\x33\x00', b'\x11\x22\x00\x33'),
    'Non canonical trailing empty block',
    DecodeCase(b'\x02\x11\x01\x00', b'\x11\x00'),
    'Non canonical empty block after max',
    DecodeCase(b'\xff' + b'\xaa' * 254 + b'\x01\x00', b'\xaa' * 254),
    'Empty input',
    DecodeCase(b'', None),
    'Bare delimiter',
    DecodeCase(b'\x00', None),
    'Missing delimiter',
    DecodeCase(b'\x01', None),
    'Zero inside block',
    DecodeCase(b'\x03\x11\x00\x00', None),
    'Zero inside block 2',
    DecodeCase(b'\x02\x11\x00\x00', None),
    'Zero code after block',
    DecodeCase(b'\x02\x11\x00\x01\x00', None),
    'Truncated block',
    DecodeCase(b'\x05\x11\x22\x00', None),
    'Truncated single code',
    DecodeCase(b'\x02\x00', None),
    'Truncated max block',
    DecodeCase(b'\xff' + b'\xaa' * 253 + b'\x00', None),
    'Truncated max block short',
    DecodeCase(b'\xff' + _nonzero_255(100) + b'\x00', None),
)

_GOOD_PAYLOAD = b'hi\x00there\n'
_GOOD_WIRE = b'\x00\x03hi\x07there\n\x00'
_GOOD2_PAYLOAD = b'\xab' * 40
_GOOD2_WIRE = b'\x00\x29' + _GOOD2_PAYLOAD + b'\x00'
_BIG_PAYLOAD = _nonzero_254(600)
_BIG_WIRE = (
    b'\x00\xff'
    + _BIG_PAYLOAD[:254]
    + b'\xff'
    + _BIG_PAYLOAD[254:508]
    + b'\x5d'
    + _BIG_PAYLOAD[508:]
    + b'\x00'
)

DECODER_TEST_CASES: tuple[GroupOrTest[DecoderCase], ...] = (
    'Empty input',
    DecoderCase(1024, b'', ()),
    'Idle delimiters only',
    DecoderCase(1024, b'\x00\x00\x00', ()),
    'Single frame',
    DecoderCase(1024, _GOOD_WIRE, (ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),)),
    'Two frames back to back',
    DecoderCase(
        1024,
        _GOOD_WIRE + _GOOD2_WIRE,
        (
            ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),
            ExpectedDecoderFrame.ok(_GOOD2_PAYLOAD),
        ),
    ),
    'Two frames shared delimiter',
    DecoderCase(
        1024,
        _GOOD_WIRE[:-1] + _GOOD2_WIRE,
        (
            ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),
            ExpectedDecoderFrame.ok(_GOOD2_PAYLOAD),
        ),
    ),
    'Empty payload frame',
    DecoderCase(1024, b'\x00\x01\x00', (ExpectedDecoderFrame.ok(b''),)),
    'Single zero frame',
    DecoderCase(1024, b'\x00\x01\x01\x00', (ExpectedDecoderFrame.ok(b'\x00'),)),
    'Truncated mid run then frame',
    DecoderCase(
        1024,
        _GOOD_WIRE[:5] + b'\x00' + _GOOD2_WIRE,
        (
            ExpectedDecoderFrame.error(cobs.DecodeStatus.DATA_LOSS),
            ExpectedDecoderFrame.ok(_GOOD2_PAYLOAD),
        ),
    ),
    'Incomplete frame reports nothing',
    DecoderCase(1024, _GOOD_WIRE[:-1], ()),
    'No leading delimiter',
    DecoderCase(
        1024, _GOOD_WIRE[1:], (ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),)
    ),
    'Boundary abort then frame',
    DecoderCase(
        1024,
        b'\x00\xff\x00' + _GOOD_WIRE,
        (
            ExpectedDecoderFrame.error(cobs.DecodeStatus.DATA_LOSS),
            ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),
        ),
    ),
    'Mid run abort then frame',
    DecoderCase(
        1024,
        b'\x00\x05\x11\x22\x00' + _GOOD_WIRE,
        (
            ExpectedDecoderFrame.error(cobs.DecodeStatus.DATA_LOSS),
            ExpectedDecoderFrame.ok(_GOOD_PAYLOAD),
        ),
    ),
    'Big frame',
    DecoderCase(1024, _BIG_WIRE, (ExpectedDecoderFrame.ok(_BIG_PAYLOAD),)),
    'Max run unterminated',
    DecoderCase(
        1024,
        b'\x00\xff' + b'A' * 254 + b'\x00',
        (ExpectedDecoderFrame.ok(b'A' * 254),),
    ),
    'Max run then empty block',
    DecoderCase(
        1024,
        b'\x00\xff' + b'A' * 254 + b'\x01\x00',
        (ExpectedDecoderFrame.ok(b'A' * 254),),
    ),
    'Buffer 9 payload 9 fits',
    DecoderCase(
        9,
        b'\x00\x0a' + b'\xaa' * 9 + b'\x00',
        (ExpectedDecoderFrame.ok(b'\xaa' * 9),),
    ),
    'Buffer 9 payload 10 overflows',
    DecoderCase(
        9,
        b'\x00\x0b' + b'\xaa' * 10 + b'\x00',
        (ExpectedDecoderFrame.error(cobs.DecodeStatus.RESOURCE_EXHAUSTED),),
    ),
    'Buffer 9 zeros 9 fits',
    DecoderCase(
        9,
        b'\x00' + b'\x01' * 10 + b'\x00',
        (ExpectedDecoderFrame.ok(b'\x00' * 9),),
    ),
    'Buffer 9 zeros 10 overflows',
    DecoderCase(
        9,
        b'\x00' + b'\x01' * 11 + b'\x00',
        (ExpectedDecoderFrame.error(cobs.DecodeStatus.RESOURCE_EXHAUSTED),),
    ),
    'Buffer 0 empty payload fits',
    DecoderCase(0, b'\x00\x01\x00', (ExpectedDecoderFrame.ok(b''),)),
    'Buffer 600 big fits',
    DecoderCase(600, _BIG_WIRE, (ExpectedDecoderFrame.ok(_BIG_PAYLOAD),)),
    'Buffer 599 big overflows',
    DecoderCase(
        599,
        _BIG_WIRE,
        (ExpectedDecoderFrame.error(cobs.DecodeStatus.RESOURCE_EXHAUSTED),),
    ),
)

_CPP_HEADER = """\
#include "pw_cobs/cobs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "pw_bytes/array.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::cobs {
namespace {

template <size_t kDecodedSize>
void CheckEncodeCase(ConstByteSpan decoded, ConstByteSpan expected_encoded) {
  std::array<std::byte, MaxEncodedSize(kDecodedSize)> encoded_buf{};
  const Result<ByteSpan> encoded = Encode(decoded, encoded_buf);
  ASSERT_EQ(OkStatus(), encoded.status());
  ASSERT_EQ(expected_encoded.size(), encoded->size());
  EXPECT_EQ(
      std::memcmp(
          expected_encoded.data(), encoded->data(), expected_encoded.size()),
      0);

  std::array<std::byte, kDecodedSize + 1> decoded_buf{};
  const Result<ByteSpan> round_trip = Decode(expected_encoded, decoded_buf);
  ASSERT_EQ(OkStatus(), round_trip.status());
  ASSERT_EQ(decoded.size(), round_trip->size());
  if (!decoded.empty()) {
    EXPECT_EQ(
        std::memcmp(decoded.data(), round_trip->data(), decoded.size()), 0);
  }

  std::array<std::byte, MaxEncodedSize(kDecodedSize)> in_place{};
  if (!decoded.empty()) {
    std::memcpy(in_place.data() + 1, decoded.data(), decoded.size());
  }
  const Result<ByteSpan> in_place_res = EncodeInPlace(in_place, decoded.size());
  ASSERT_EQ(OkStatus(), in_place_res.status());
  ASSERT_EQ(expected_encoded.size(), in_place_res->size());
  EXPECT_EQ(std::memcmp(expected_encoded.data(),
                        in_place_res->data(),
                        expected_encoded.size()),
            0);

  EncodeBuffer<kDecodedSize> enc_buf(decoded);
  const ConstByteSpan buf_encoded = enc_buf.Encode();
  ASSERT_EQ(expected_encoded.size(), buf_encoded.size());
  EXPECT_EQ(std::memcmp(expected_encoded.data(),
                        buf_encoded.data(),
                        expected_encoded.size()),
            0);

  std::array<std::byte, MaxEncodedSize(kDecodedSize)> streamed{};
  size_t streamed_len = 0;
  StreamerBuffer<> streamer;
  ConstByteSpan rem = decoded;
  while (!rem.empty()) {
    rem = streamer.Write(rem);
    if (!rem.empty()) {
      const ConstByteSpan chunk = streamer.Flush();
      std::memcpy(streamed.data() + streamed_len, chunk.data(), chunk.size());
      streamed_len += chunk.size();
    }
  }
  const ConstByteSpan tail = streamer.Finish();
  std::memcpy(streamed.data() + streamed_len, tail.data(), tail.size());
  streamed_len += tail.size();
  ASSERT_EQ(expected_encoded.size(), streamed_len);
  EXPECT_EQ(
      std::memcmp(
          expected_encoded.data(), streamed.data(), expected_encoded.size()),
      0);
}

template <size_t kEncodedSize>
void CheckDecodeOk(ConstByteSpan encoded, ConstByteSpan expected_decoded) {
  std::array<std::byte, kEncodedSize + 1> decoded_buf{};
  const Result<ByteSpan> result = Decode(encoded, decoded_buf);
  ASSERT_EQ(OkStatus(), result.status());
  ASSERT_EQ(expected_decoded.size(), result->size());
  if (!expected_decoded.empty()) {
    EXPECT_EQ(std::memcmp(expected_decoded.data(),
                          result->data(),
                          expected_decoded.size()),
              0);
  }
}

template <size_t kEncodedSize>
void CheckDecodeError(ConstByteSpan encoded, Status expected_status) {
  std::array<std::byte, kEncodedSize + 1> decoded_buf{};
  const Result<ByteSpan> result = Decode(encoded, decoded_buf);
  EXPECT_EQ(expected_status, result.status());
}

struct ExpectedFrame {
  Status status;
  ConstByteSpan decoded;

  static constexpr ExpectedFrame Ok(ConstByteSpan data) {
    return ExpectedFrame{OkStatus(), data};
  }

  static constexpr ExpectedFrame Error(Status err) {
    return ExpectedFrame{err, ConstByteSpan()};
  }
};

void CheckDeliveredFrame(const Result<ByteSpan>& actual,
                         const ExpectedFrame& expected) {
  EXPECT_EQ(expected.status, actual.status());
  if (expected.status.ok() && actual.ok()) {
    ASSERT_EQ(expected.decoded.size(), actual->size());
    if (!expected.decoded.empty()) {
      EXPECT_EQ(std::memcmp(expected.decoded.data(),
                            actual->data(),
                            expected.decoded.size()),
                0);
    }
  }
}

template <size_t kBufferSize>
void CheckDecoderCase(ConstByteSpan wire,
                      span<const ExpectedFrame> expected_frames) {
  // 1. Bulk process.
  {
    DecoderBuffer<kBufferSize> decoder;
    size_t index = 0;
    decoder.Process(wire, [&](const Result<ByteSpan>& frame) {
      ASSERT_LT(index, expected_frames.size());
      CheckDeliveredFrame(frame, expected_frames[index]);
      ++index;
    });
    EXPECT_EQ(expected_frames.size(), index);
  }

  // 2. Byte-at-a-time process.
  {
    DecoderBuffer<kBufferSize> decoder;
    size_t index = 0;
    for (std::byte b : wire) {
      const Result<ByteSpan> frame = decoder.Process(b);
      if (!frame.status().IsUnavailable()) {
        ASSERT_LT(index, expected_frames.size());
        CheckDeliveredFrame(frame, expected_frames[index]);
        ++index;
      }
    }
    EXPECT_EQ(expected_frames.size(), index);
  }

  // 3. Two-part split at offsets across the wire.
  const size_t step = wire.size() > 64 ? 7 : 1;
  for (size_t split = 0; split <= wire.size(); split += step) {
    DecoderBuffer<kBufferSize> decoder;
    size_t index = 0;
    auto on_frame = [&](const Result<ByteSpan>& frame) {
      ASSERT_LT(index, expected_frames.size());
      CheckDeliveredFrame(frame, expected_frames[index]);
      ++index;
    };
    const size_t cut = std::min(split, wire.size());
    decoder.Process(wire.first(cut), on_frame);
    decoder.Process(wire.subspan(cut), on_frame);
    EXPECT_EQ(expected_frames.size(), index);
  }
}
"""

_CPP_FOOTER = """\
}  // namespace
}  // namespace pw::cobs"""


def _define_py_encode_test(ctx: Context[EncodeCase]) -> PyTest:
    decoded, encoded = ctx.test_case

    def test(self) -> None:
        self.assertEqual(cobs.encode(decoded), encoded)
        self.assertEqual(cobs.decode(encoded), decoded)

        streamer = cobs.Streamer()
        rem = decoded
        streamed = bytearray()
        while rem:
            rem = streamer.write(rem)
            if rem:
                streamed += streamer.flush()
        streamed += streamer.finish()
        self.assertEqual(bytes(streamed), encoded)

    return test


def _cc_encode_test(ctx: Context[EncodeCase]) -> Iterator[str]:
    decoded, encoded = ctx.test_case
    yield f'TEST(CobsGeneratedEncode, {ctx.cc_name()}) {{'
    yield (
        f'  static constexpr auto kDecoded = '
        f'bytes::String({cpp.string_literal(decoded)});'
    )
    yield (
        f'  static constexpr auto kEncoded = '
        f'bytes::String({cpp.string_literal(encoded)});'
    )
    yield f'  CheckEncodeCase<{len(decoded)}>(kDecoded, kEncoded);'
    yield '}'


def _define_py_decode_test(ctx: Context[DecodeCase]) -> PyTest:
    encoded, decoded = ctx.test_case

    def test(self) -> None:
        if decoded is not None:
            self.assertEqual(cobs.decode(encoded), decoded)
        else:
            with self.assertRaises(cobs.CobsDecodeError):
                cobs.decode(encoded)

    return test


def _cc_decode_test(ctx: Context[DecodeCase]) -> Iterator[str]:
    encoded, decoded = ctx.test_case
    yield f'TEST(CobsGeneratedDecode, {ctx.cc_name()}) {{'
    yield (
        f'  static constexpr auto kEncoded = '
        f'bytes::String({cpp.string_literal(encoded)});'
    )
    if decoded is not None:
        yield (
            f'  static constexpr auto kDecoded = '
            f'bytes::String({cpp.string_literal(decoded)});'
        )
        yield f'  CheckDecodeOk<{len(encoded)}>(kEncoded, kDecoded);'
    else:
        yield (
            f'  CheckDecodeError<{len(encoded)}>(kEncoded, Status::DataLoss());'
        )
    yield '}'


def _to_expected_frame(frame: cobs.DecodedFrame) -> ExpectedDecoderFrame:
    return ExpectedDecoderFrame(frame.status, frame.data)


def _define_py_decoder_test(ctx: Context[DecoderCase]) -> PyTest:
    buffer_size, wire, expected = ctx.test_case
    expected_list = list(expected)

    def test(self) -> None:
        decoder = cobs.Decoder(buffer_size=buffer_size)
        actual_bulk = [_to_expected_frame(f) for f in decoder.process(wire)]
        self.assertEqual(actual_bulk, expected_list)

        decoder = cobs.Decoder(buffer_size=buffer_size)
        actual_byte = [
            _to_expected_frame(f)
            for b in wire
            if (f := decoder.process_byte(b)) is not None
        ]
        self.assertEqual(actual_byte, expected_list)

        step = 7 if len(wire) > 64 else 1
        for split in range(0, len(wire) + 1, step):
            decoder = cobs.Decoder(buffer_size=buffer_size)
            frames = list(decoder.process(wire[:split]))
            frames += decoder.process(wire[split:])
            self.assertEqual(
                [_to_expected_frame(f) for f in frames],
                expected_list,
                f'{ctx.group}: split at {split}',
            )

    return test


_STATUS_CC = {
    cobs.DecodeStatus.DATA_LOSS: 'Status::DataLoss()',
    cobs.DecodeStatus.RESOURCE_EXHAUSTED: 'Status::ResourceExhausted()',
}


def _cc_decoder_test(ctx: Context[DecoderCase]) -> Iterator[str]:
    buffer_size, wire, expected = ctx.test_case
    yield f'TEST(CobsGeneratedDecoder, {ctx.cc_name()}) {{'
    yield (
        f'  static constexpr auto kWire = '
        f'bytes::String({cpp.string_literal(wire)});'
    )
    if not expected:
        yield (
            f'  CheckDecoderCase<{buffer_size}>('
            f'kWire, span<const ExpectedFrame>());'
        )
        yield '}'
        return

    for i, frame in enumerate(expected):
        if frame.status is cobs.DecodeStatus.OK:
            yield (
                f'  static constexpr auto kFrame{i} = '
                f'bytes::String({cpp.string_literal(frame.decoded)});'
            )

    yield (
        f'  static constexpr std::array<ExpectedFrame, {len(expected)}> '
        f'kExpected = {{'
    )
    for i, frame in enumerate(expected):
        if frame.status is cobs.DecodeStatus.OK:
            yield f'      ExpectedFrame::Ok(kFrame{i}),'
        else:
            yield f'      ExpectedFrame::Error({_STATUS_CC[frame.status]}),'
    yield '  };'
    yield f'  CheckDecoderCase<{buffer_size}>(kWire, kExpected);'
    yield '}'


_ENCODE_TESTS = TestGenerator(
    ENCODE_TEST_CASES,
    cc_test=(_cc_encode_test, _CPP_HEADER, ''),
)

_DECODE_TESTS = TestGenerator(
    DECODE_TEST_CASES,
    cc_test=(_cc_decode_test, '', ''),
)

_DECODER_TESTS = TestGenerator(
    DECODER_TEST_CASES,
    cc_test=(_cc_decoder_test, '', _CPP_FOOTER),
)

CobsGeneratedEncodeTest = _ENCODE_TESTS.python_tests(
    'CobsGeneratedEncodeTest', _define_py_encode_test
)
CobsGeneratedDecodeTest = _DECODE_TESTS.python_tests(
    'CobsGeneratedDecodeTest', _define_py_decode_test
)
CobsGeneratedDecoderTest = _DECODER_TESTS.python_tests(
    'CobsGeneratedDecoderTest', _define_py_decoder_test
)

if __name__ == '__main__':
    generated_tests.main(_ENCODE_TESTS, _DECODE_TESTS, _DECODER_TESTS)
