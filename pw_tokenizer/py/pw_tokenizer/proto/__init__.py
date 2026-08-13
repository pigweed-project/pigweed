# Copyright 2021 The Pigweed Authors
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
"""Utilities for working with tokenized fields in protobufs."""

import re
from typing import Iterator

from google.protobuf.descriptor import FieldDescriptor
from google.protobuf.message import Message

from pw_tokenizer_proto import options_pb2
from pw_tokenizer import detokenize, encode

# Regular expression to match ANSI escape sequences (both 2-character escape
# codes and Control Sequence Introducer (CSI) sequences).
#
# Structure of CSI sequences (see ECMA-48):
#   \x1b\[      - CSI prefix
#   [\x30-\x3F]* - Parameter bytes (ASCII 0x30 to 0x3F, '0' to '?')
#   [\x20-\x2F]* - Intermediate bytes (ASCII 0x20 to 0x2F, space to '/')
#   [\x40-\x7E]  - Final byte (ASCII 0x40 to 0x7E, '@' to '~')
#
# The 2-character escape codes match \x1B followed by a character in the range
# 0x40 to 0x5F (excluding '[', which is 0x5B and starts a CSI sequence).
# This is represented by [\x40-\x5A\x5C-\x5F].
_CSI_ESCAPE = re.compile(
    r'\x1B(?:[\x40-\x5A\x5C-\x5F]|\[[\x30-\x3F]*[\x20-\x2F]*[\x40-\x7E])'
)


def _tokenized_fields(proto: Message) -> Iterator[FieldDescriptor]:
    for field in proto.DESCRIPTOR.fields:
        extensions = field.GetOptions().Extensions
        if (
            options_pb2.format in extensions
            and extensions[options_pb2.format]
            == options_pb2.TOKENIZATION_OPTIONAL
        ):
            yield field


def decode_optionally_tokenized(
    detokenizer: detokenize.Detokenizer | None,
    data: bytes,
) -> str:
    """Decodes data that may be plain text or binary / Base64 tokenized text.

    Args:
      detokenizer: detokenizer to use; if `None`, binary logs as Base64 encoded
      data: encoded text or binary data
    """
    prefix = detokenizer.prefix if detokenizer else encode.NESTED_TOKEN_PREFIX

    if detokenizer:
        # Try detokenizing as binary.
        result = detokenizer.detokenize(data)
        if result.best_result() is not None:
            # Rather than just returning the detokenized string, continue
            # detokenization in case recursive Base64 detokenization is needed.
            data = str(result).encode()

    # Attempt to decode as UTF-8.
    try:
        text = data.decode()
    except UnicodeDecodeError:
        # Not UTF-8. Assume the token is unknown or the data is corrupt.
        return encode.prefixed_base64(data, prefix)

    # See if the string contains nested messages.
    if detokenizer:
        detokenized = detokenizer.detokenize_text(data)
        if detokenized != data:  # If detokenized successfully, use the result.
            return detokenized.decode()

    # Attempt to determine whether this is an unknown token or plain text.
    # Any string with only printable or whitespace characters is plain text.
    # Strip ANSI escape sequences before checking printability.
    cleaned_text = _CSI_ESCAPE.sub('', text)
    if ''.join(cleaned_text.split()).isprintable():
        return text

    # Assume this field is tokenized data that could not be decoded.
    return encode.prefixed_base64(data, prefix)


def detokenize_fields(
    detokenizer: detokenize.Detokenizer | None,
    proto: Message,
) -> None:
    """Detokenizes fields annotated as tokenized in the given proto.

    The fields are replaced with their detokenized version in the proto.
    Tokenized fields are bytes fields, so the detokenized string is stored as
    bytes. Call .decode() to convert the detokenized string from bytes to str.
    """
    for field in _tokenized_fields(proto):
        decoded = decode_optionally_tokenized(
            detokenizer, getattr(proto, field.name)
        )
        setattr(proto, field.name, decoded.encode())
