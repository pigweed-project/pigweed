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
"""Python implementation of pw_cobs Consistent Overhead Byte Stuffing."""

from pw_cobs.cobs import (
    ABORT_SEQUENCE,
    MAX_RUN_LENGTH,
    MIN_BUFFER_SIZE,
    Chunk,
    CobsDecodeError,
    DecodedFrame,
    Decoder,
    DecodeStatus,
    Streamer,
    decode,
    encode,
    max_decoded_size,
    max_encoded_size,
)

cobs_decode = decode
cobs_encode = encode

__all__ = [
    'ABORT_SEQUENCE',
    'MAX_RUN_LENGTH',
    'MIN_BUFFER_SIZE',
    'Chunk',
    'CobsDecodeError',
    'DecodeStatus',
    'DecodedFrame',
    'Decoder',
    'Streamer',
    'cobs_decode',
    'cobs_encode',
    'decode',
    'encode',
    'max_decoded_size',
    'max_encoded_size',
]
