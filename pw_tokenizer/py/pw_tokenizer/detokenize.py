#!/usr/bin/env python3
# Copyright 2020 The Pigweed Authors
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
r"""Decodes and detokenizes strings from binary or Base64 input.

The main class provided by this module is the Detokenize class. To use it,
construct it with the path to an ELF or CSV database, a tokens.Database,
or a file object for an ELF file or CSV. Then, call the detokenize method with
encoded messages, one at a time. The detokenize method returns a
DetokenizedString object with the result.

For example::

  from pw_tokenizer import detokenize

  detok = detokenize.Detokenizer('path/to/firmware/image.elf')
  print(detok.detokenize(b'\x12\x34\x56\x78\x03hi!'))

This module also provides a command line interface for decoding and detokenizing
messages from a file or stdin.
"""

import argparse
import base64
import binascii
from concurrent.futures import Executor, ThreadPoolExecutor
import enum
import io
import logging
import os
from pathlib import Path
import re
import string
import struct
import sys
import threading
import time
from typing import (
    AnyStr,
    BinaryIO,
    Callable,
    Iterable,
    Iterator,
    Match,
    NamedTuple,
    Pattern,
)

try:
    from pw_tokenizer import database, decode, encode, tokens
except ImportError:
    # Append this path to the module search path to allow running this module
    # without installing the pw_tokenizer package.
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from pw_tokenizer import database, decode, encode, tokens

_LOG = logging.getLogger('pw_tokenizer')

ENCODED_TOKEN = struct.Struct('<I')
_BASE64_CHARS = string.ascii_letters + string.digits + '+/-_='
DEFAULT_RECURSION = 5
NESTED_TOKEN_BASE_PREFIX = encode.NESTED_TOKEN_BASE_PREFIX.encode()
NESTED_DOMAIN_START_PREFIX = encode.NESTED_DOMAIN_START_PREFIX.encode()
NESTED_DOMAIN_END_PREFIX = encode.NESTED_DOMAIN_END_PREFIX.encode()

_BASE8_TOKEN_REGEX = rb'(?P<base8>[0-7]{11})'
_BASE10_TOKEN_REGEX = rb'(?P<base10>[0-9]{10})'
_BASE16_TOKEN_REGEX = rb'(?P<base16>[A-Fa-f0-9]{8})'
_BASE64_TOKEN_REGEX = (
    rb'(?P<base64>'
    # Tokenized Base64 contains 0 or more blocks of four Base64 chars.
    rb'(?:[A-Za-z0-9+/\-_]{4})*'
    # The last block of 4 chars may have one or two padding chars (=).
    rb'(?:[A-Za-z0-9+/\-_]{3}=|[A-Za-z0-9+/\-_]{2}==)?'
    rb')'
)
_NESTED_TOKEN_FORMATS = (
    _BASE8_TOKEN_REGEX,
    _BASE10_TOKEN_REGEX,
    _BASE16_TOKEN_REGEX,
    _BASE64_TOKEN_REGEX,
)


def _token_regex(prefix: str) -> Pattern[bytes]:
    """Returns a regular expression for prefixed tokenized strings."""
    return re.compile(
        # Tokenized strings start with the prefix character ($).
        re.escape(prefix.encode())
        # Optional; no domain specifier defaults to (empty) domain.
        # Brackets ({}) specifies domain string
        + rb'(?P<domainspec>('
        + NESTED_DOMAIN_START_PREFIX
        + rb'(?P<domain>\s*|\s*[a-zA-Z_:][a-zA-Z0-9_:\s]*)'
        + NESTED_DOMAIN_END_PREFIX
        + rb'))?'
        # Optional; no base specifier defaults to BASE64.
        # Hash (#) with no number specified defaults to Base-16.
        + rb'(?P<basespec>(?P<base>[0-9]*)?'
        + NESTED_TOKEN_BASE_PREFIX
        + rb')?'
        # Match one of the following token formats.
        + rb'('
        + rb'|'.join(_NESTED_TOKEN_FORMATS)
        + rb')'
    )


class DetokenizedString:
    """A detokenized string, with all results if there are collisions."""

    def __init__(
        self,
        token: int | None,
        format_string_entries: Iterable[tuple],
        encoded_message: bytes,
        show_errors: bool = False,
        recursive_detokenize: Callable[[str], str] | None = None,
    ):
        self.token = token
        self.encoded_message = encoded_message
        self._show_errors = show_errors

        self.successes: list[decode.FormattedString] = []
        self.failures: list[decode.FormattedString] = []

        decode_attempts: list[tuple[tuple, decode.FormattedString]] = []

        for entry, fmt in format_string_entries:
            result = fmt.format(
                encoded_message[ENCODED_TOKEN.size :], show_errors
            )
            if recursive_detokenize:
                result = decode.FormattedString(
                    recursive_detokenize(result.value),
                    result.args,
                    result.remaining,
                )
            decode_attempts.append((result.score(entry.date_removed), result))

        # Sort the attempts by the score so the most likely results are first.
        decode_attempts.sort(key=lambda value: value[0], reverse=True)

        # Split out the successesful decodes from the failures.
        for score, result in decode_attempts:
            if score[0]:
                self.successes.append(result)
            else:
                self.failures.append(result)

    def ok(self) -> bool:
        """True if exactly one string decoded the arguments successfully."""
        return len(self.successes) == 1

    def matches(self) -> list[decode.FormattedString]:
        """Returns the strings that matched the token, best matches first."""
        return self.successes + self.failures

    def best_result(self) -> decode.FormattedString | None:
        """Returns the string and args for the most likely decoded string."""
        for string_and_args in self.matches():
            return string_and_args

        return None

    def error_message(self) -> str:
        """If detokenization failed, returns a descriptive message."""
        if self.ok():
            return ''

        if not self.matches():
            if self.token is None:
                return 'missing token'

            return 'unknown token {:08x}'.format(self.token)

        if len(self.matches()) == 1:
            return 'decoding failed for {!r}'.format(self.matches()[0].value)

        return '{} matches'.format(len(self.matches()))

    def __str__(self) -> str:
        """Returns the string for the most likely result."""
        result = self.best_result()
        if result:
            return result[0]

        if self._show_errors:
            return '<[ERROR: {}|{!r}]>'.format(
                self.error_message(), self.encoded_message
            )

        # Display the string as prefixed Base64 if it cannot be decoded.
        return encode.prefixed_base64(self.encoded_message)

    def __repr__(self) -> str:
        if self.ok():
            message = repr(str(self))
        else:
            message = 'ERROR: {}|{!r}'.format(
                self.error_message(), self.encoded_message
            )

        return '{}({})'.format(type(self).__name__, message)


class _TokenizedFormatString(NamedTuple):
    entry: tokens.TokenizedStringEntry
    format: decode.FormatString


class Detokenizer:
    """Main detokenization class; detokenizes strings and caches results."""

    def __init__(
        self,
        *token_database_or_elf,
        show_errors: bool = False,
        prefix: str | bytes = encode.NESTED_TOKEN_PREFIX,
    ):
        """Decodes and detokenizes binary messages.

        Args:
          *token_database_or_elf: a path or file object for an ELF or CSV
              database, a tokens.Database, or an elf_reader.Elf
          prefix: one-character byte string that signals the start of a message
          show_errors: if True, an error message is used in place of the %
              conversion specifier when an argument fails to decode
        """
        self.show_errors = show_errors
        self._prefix = prefix if isinstance(prefix, str) else prefix.decode()
        self._token_regex = _token_regex(self._prefix)
        self._database_lock = threading.Lock()

        # Cache FormatStrings for faster lookup & formatting.
        self._cache: dict[int, list[_TokenizedFormatString]] = {}

        self._initialize_database(token_database_or_elf)

    @property
    def prefix(self) -> str:
        return self._prefix

    def _initialize_database(self, token_sources: Iterable) -> None:
        with self._database_lock:
            self.database = database.load_token_database(*token_sources)
            self._cache.clear()

    def lookup(self, token: int) -> list[_TokenizedFormatString]:
        """Returns (TokenizedStringEntry, FormatString) list for matches."""
        with self._database_lock:
            try:
                return self._cache[token]
            except KeyError:
                format_strings = [
                    _TokenizedFormatString(
                        entry, decode.FormatString(str(entry))
                    )
                    for entry in self.database.token_to_entries[token]
                ]
                self._cache[token] = format_strings
                return format_strings

    def detokenize(
        self,
        encoded_message: bytes,
        domain: str | None = None,
        recursion: int = DEFAULT_RECURSION,
    ) -> DetokenizedString:
        """Decodes and detokenizes a message as a DetokenizedString."""
        if not encoded_message:
            return DetokenizedString(
                None, (), encoded_message, self.show_errors
            )

        # Pad messages smaller than ENCODED_TOKEN.size with zeroes to support
        # tokens smaller than a uint32. Messages with arguments must always use
        # a full 32-bit token.
        missing_token_bytes = ENCODED_TOKEN.size - len(encoded_message)
        if missing_token_bytes > 0:
            encoded_message += b'\0' * missing_token_bytes

        (token,) = ENCODED_TOKEN.unpack_from(encoded_message)

        recursive_detokenize = None
        if recursion > 0:
            recursive_detokenize = self._detokenize_nested_callback(recursion)

        if domain is None:
            entries = self.lookup(token)
        else:
            entries = [
                _TokenizedFormatString(entry, decode.FormatString(str(entry)))
                for entry in self.database.domains[domain][token]
            ]

        return DetokenizedString(
            token,
            entries,
            encoded_message,
            self.show_errors,
            recursive_detokenize,
        )

    def detokenize_text(
        self,
        data: AnyStr,
        recursion: int = DEFAULT_RECURSION,
    ) -> AnyStr:
        """Decodes and replaces prefixed Base64 messages in the provided data.

        Args:
          data: the binary data to decode
          recursion: how many levels to recursively decode

        Returns:
          copy of the data with all recognized tokens decoded
        """
        return self._detokenize_nested_callback(recursion)(data)

    def detokenize_text_to_file(
        self,
        data: AnyStr,
        output: BinaryIO,
        recursion: int = DEFAULT_RECURSION,
    ) -> None:
        """Decodes prefixed Base64 messages in data; decodes to output file."""
        output.write(self._detokenize_nested(data, recursion))

    def detokenize_text_live(
        self,
        input_file: io.RawIOBase | BinaryIO,
        output: BinaryIO,
        recursion: int = DEFAULT_RECURSION,
    ) -> None:
        """Reads chars one-at-a-time, decoding messages; SLOW for big files."""

        def transform(data: bytes) -> bytes:
            return self._detokenize_nested(data.decode(), recursion)

        for message in NestedMessageParser(
            self._prefix, _BASE64_CHARS
        ).transform_io(input_file, transform):
            output.write(message)

            # Flush each line to prevent delays when piping between processes.
            if b'\n' in message:
                output.flush()

    def _detokenize_nested_callback(
        self,
        recursion: int,
    ) -> Callable[[AnyStr], AnyStr]:
        """Returns a function that replaces all tokens for a given string."""

        def detokenize(message: AnyStr) -> AnyStr:
            result = self._detokenize_nested(message, recursion)
            return result.decode() if isinstance(message, str) else result

        return detokenize

    def _detokenize_nested(
        self,
        message: str | bytes,
        recursion: int,
    ) -> bytes:
        """Returns the message with recognized tokens replaced.

        Message data is internally handled as bytes regardless of input message
        type and returns the result as bytes.
        """
        # A unified format across the token types is required for regex
        # consistency.
        message = message.encode() if isinstance(message, str) else message

        if not self.database:
            return message

        result = message
        for _ in range(recursion - 1):
            result = self._token_regex.sub(self._detokenize_scan, result)

            if result == message:
                return result
        return result

    def _detokenize_scan(self, match: Match[bytes]) -> bytes:
        """Decodes prefixed tokens for one of multiple formats."""
        basespec = match.group('basespec')
        base = match.group('base')
        domain = match.group('domain')

        if domain is None:
            domain = tokens.DEFAULT_DOMAIN
        else:
            domain = domain.decode()

        domain = ''.join(domain.split())

        if not basespec or (base == b'64'):
            return self._detokenize_once_base64(
                match.group(0), domain, match.group('base64')
            )

        if not base:
            base = b'16'

        token = match.group('base' + base.decode())
        if not token:
            return match.group(0)

        return self._detokenize_once_token(match.group(0), domain, token, base)

    def _detokenize_once_token(
        self, original: bytes, domain: str, token_str: bytes, base: bytes
    ) -> bytes:
        """Performs lookup on a plain token"""
        token = int(token_str, int(base))
        entries = self.database.domains[domain][token]

        if len(entries) == 1:
            return str(entries[0]).encode()

        # TODO(gschen): improve token collision reporting

        return original

    def _detokenize_once_base64(
        self,
        original: bytes,
        domain: str,
        base64_message: bytes,
    ) -> bytes:
        """Performs lookup on a Base64 token"""
        if not base64_message:
            return original

        try:
            detokenized_string = self.detokenize(
                base64.b64decode(base64_message, validate=True),
                domain=domain,
                recursion=0,
            )

            if detokenized_string.matches():
                return str(detokenized_string).encode()

        except binascii.Error:
            pass

        return original


class AutoUpdatingDetokenizer(Detokenizer):
    """Loads and updates a detokenizer from database paths."""

    class _DatabasePath:
        """Tracks the modified time of a path or file object."""

        def __init__(self, path: Path | str) -> None:
            self.path, self.domain = database.parse_domain(path)
            self._modified_time: float | None = self._last_modified_time()

        def updated(self) -> bool:
            """True if the path has been updated since the last call."""
            modified_time = self._last_modified_time()
            if modified_time is None or modified_time == self._modified_time:
                return False

            self._modified_time = modified_time
            return True

        def _last_modified_time(self) -> float | None:
            if self.path.is_dir():
                mtime = -1.0
                for child in self.path.glob(tokens.DIR_DB_GLOB):
                    mtime = max(mtime, os.path.getmtime(child))
                return mtime if mtime >= 0 else None

            try:
                return os.path.getmtime(self.path)
            except FileNotFoundError:
                return None

        def load(self) -> tokens.Database:
            try:
                if self.domain is not None:
                    return database.load_token_database(
                        self.path, domain=self.domain
                    )
                return database.load_token_database(self.path)
            except FileNotFoundError:
                return database.load_token_database()

    def __init__(
        self,
        *paths_or_files: Path | str,
        min_poll_period_s: float = 1.0,
        pool: Executor = ThreadPoolExecutor(max_workers=1),
        prefix: str | bytes = encode.NESTED_TOKEN_PREFIX,
        extra_databases: Iterable = (),
    ) -> None:
        """Constructs the AutoUpdatingDetokenizer.

        Args:
          *paths_or_files: a path or string to an ELF or CSV path with the token
              database
          min_poll_period_s: the minimum time between polls in seconds
          pool: thread executor pool
          prefix: a one-character byte string that signals the start of a
              message
          extra_databases: an optional tokens.Database list that is always
              merged with the paths_or_files.
              Note: adding or removing tokens form this database is not visible
              to the underlying Detokenizer. However, these databases are
              re-loaded when a path changes.
        """
        self.paths = tuple(self._DatabasePath(path) for path in paths_or_files)
        self.min_poll_period_s = min_poll_period_s
        self._last_checked_time: float = time.time()
        # Thread pool to use for loading the databases. Limit to a single
        # worker since this is low volume and not time critical.
        self._pool = pool
        self._extra_databases = list(extra_databases)
        super().__init__(
            *([path.load() for path in self.paths] + self._extra_databases),
            prefix=prefix,
        )

    def __del__(self) -> None:
        if hasattr(self, '_pool'):
            self._pool.shutdown(wait=False)

    def _reload_paths(self) -> None:
        self._initialize_database(
            [path.load() for path in self.paths] + self._extra_databases
        )

    def _reload_if_changed(self) -> None:
        if time.time() - self._last_checked_time >= self.min_poll_period_s:
            self._last_checked_time = time.time()

            if any(path.updated() for path in self.paths):
                _LOG.info('Changes detected; reloading token database')
                self._pool.submit(self._reload_paths)

    def lookup(self, token: int) -> list[_TokenizedFormatString]:
        self._reload_if_changed()
        return super().lookup(token)


class NestedMessageParser:
    """Parses nested tokenized messages from a byte stream or string."""

    class _State(enum.Enum):
        MESSAGE = 1
        NON_MESSAGE = 2

    def __init__(
        self,
        prefix: str | bytes = encode.NESTED_TOKEN_PREFIX,
        chars: str | bytes = _BASE64_CHARS,
    ) -> None:
        """Initializes a parser.

        Args:
            prefix: one character that signifies the start of a message (``$``).
            chars: characters allowed in a message
        """
        self._prefix = ord(prefix)

        if isinstance(chars, str):
            chars = chars.encode()

        # Store the valid message bytes as a set of byte values.
        self._message_bytes = frozenset(chars)

        if len(prefix) != 1 or self._prefix in self._message_bytes:
            raise ValueError(
                f'Invalid prefix {prefix!r}: the prefix must be a single '
                'character that is not a valid message character.'
            )

        self._buffer = bytearray()
        self._state: NestedMessageParser._State = self._State.NON_MESSAGE

    def read_messages_io(
        self, binary_io: io.RawIOBase | BinaryIO
    ) -> Iterator[tuple[bool, bytes]]:
        """Reads prefixed messages from a byte stream (BinaryIO object).

        Reads until EOF. If the stream is nonblocking (``read(1)`` returns
        ``None``), then this function returns and may be called again with the
        same IO object to continue parsing. Partial messages are preserved
        between calls.

        Yields:
            ``(is_message, contents)`` chunks.
        """
        # The read may block indefinitely, depending on the IO object.
        while (read_byte := binary_io.read(1)) != b'':
            # Handle non-blocking IO by returning when no bytes are available.
            if read_byte is None:
                return

            for byte in read_byte:
                yield from self._handle_byte(byte)

            if self._state is self._State.NON_MESSAGE:  # yield non-message byte
                yield from self._flush()

        yield from self._flush()  # Always flush after EOF
        self._state = self._State.NON_MESSAGE

    def read_messages(
        self, chunk: bytes, *, flush: bool = False
    ) -> Iterator[tuple[bool, bytes]]:
        """Reads prefixed messages from a byte string.

        This function may be called repeatedly with chunks of a stream. Partial
        messages are preserved between calls, unless ``flush=True``.

        Args:
            chunk: byte string that may contain nested messagses
            flush: whether to flush any incomplete messages after processing
                this chunk

        Yields:
            ``(is_message, contents)`` chunks.
        """
        for byte in chunk:
            yield from self._handle_byte(byte)

        if flush or self._state is self._State.NON_MESSAGE:
            yield from self._flush()

    def _handle_byte(self, byte: int) -> Iterator[tuple[bool, bytes]]:
        if self._state is self._State.MESSAGE:
            if byte not in self._message_bytes:
                yield from self._flush()
                if byte != self._prefix:
                    self._state = self._State.NON_MESSAGE
        elif self._state is self._State.NON_MESSAGE:
            if byte == self._prefix:
                yield from self._flush()
                self._state = self._State.MESSAGE
        else:
            raise NotImplementedError(f'Unsupported state: {self._state}')

        self._buffer.append(byte)

    def _flush(self) -> Iterator[tuple[bool, bytes]]:
        data = bytes(self._buffer)
        self._buffer.clear()
        if data:
            yield self._state is self._State.MESSAGE, data

    def transform_io(
        self,
        binary_io: io.RawIOBase | BinaryIO,
        transform: Callable[[bytes], bytes],
    ) -> Iterator[bytes]:
        """Yields the file with a transformation applied to the messages."""
        for is_message, chunk in self.read_messages_io(binary_io):
            yield transform(chunk) if is_message else chunk

    def transform(
        self,
        chunk: bytes,
        transform: Callable[[bytes], bytes],
        *,
        flush: bool = False,
    ) -> bytes:
        """Yields the chunk with a transformation applied to the messages.

        Partial messages are preserved between calls unless ``flush=True``.
        """
        return b''.join(
            transform(data) if is_message else data
            for is_message, data in self.read_messages(chunk, flush=flush)
        )


def _follow_and_detokenize_file(
    detokenizer: Detokenizer,
    file: BinaryIO,
    output: BinaryIO,
    poll_period_s: float = 0.01,
) -> None:
    """Polls a file to detokenize it and any appended data."""

    try:
        while True:
            data = file.read()
            if data:
                detokenizer.detokenize_text_to_file(data, output)
                output.flush()
            else:
                time.sleep(poll_period_s)
    except KeyboardInterrupt:
        pass


def _handle_base64(
    databases,
    input_file: BinaryIO,
    output: BinaryIO,
    prefix: str,
    show_errors: bool,
    follow: bool,
) -> None:
    """Handles the base64 command line option."""
    # argparse.FileType doesn't correctly handle - for binary files.
    if input_file is sys.stdin:
        input_file = sys.stdin.buffer

    if output is sys.stdout:
        output = sys.stdout.buffer

    detokenizer = Detokenizer(
        tokens.Database.merged(*databases),
        prefix=prefix,
        show_errors=show_errors,
    )

    if follow:
        _follow_and_detokenize_file(detokenizer, input_file, output)
    elif input_file.seekable():
        # Process seekable files all at once, which is MUCH faster.
        detokenizer.detokenize_text_to_file(input_file.read(), output)
    else:
        # For non-seekable inputs (e.g. pipes), read one character at a time.
        detokenizer.detokenize_text_live(input_file, output)


def _parse_args() -> argparse.Namespace:
    """Parses and return command line arguments."""

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.set_defaults(handler=lambda **_: parser.print_help())

    subparsers = parser.add_subparsers(help='Encoding of the input.')

    base64_help = 'Detokenize Base64-encoded data from a file or stdin.'
    subparser = subparsers.add_parser(
        'base64',
        description=base64_help,
        parents=[database.token_databases_parser()],
        help=base64_help,
    )
    subparser.set_defaults(handler=_handle_base64)
    subparser.add_argument(
        '-i',
        '--input',
        dest='input_file',
        type=argparse.FileType('rb'),
        default=sys.stdin.buffer,
        help='The file from which to read; provide - or omit for stdin.',
    )
    subparser.add_argument(
        '-f',
        '--follow',
        action='store_true',
        help=(
            'Detokenize data appended to input_file as it grows; similar to '
            'tail -f.'
        ),
    )
    subparser.add_argument(
        '-o',
        '--output',
        type=argparse.FileType('wb'),
        default=sys.stdout.buffer,
        help=(
            'The file to which to write the output; '
            'provide - or omit for stdout.'
        ),
    )
    subparser.add_argument(
        '-p',
        '--prefix',
        default=encode.NESTED_TOKEN_PREFIX,
        help=(
            'The one-character prefix that signals the start of a '
            'nested tokenized message. (default: $)'
        ),
    )
    subparser.add_argument(
        '-s',
        '--show_errors',
        action='store_true',
        help=(
            'Show error messages instead of conversion specifiers when '
            'arguments cannot be decoded.'
        ),
    )

    return parser.parse_args()


def main() -> int:
    args = _parse_args()

    handler = args.handler
    del args.handler

    handler(**vars(args))
    return 0


if __name__ == '__main__':
    if sys.version_info[0] < 3:
        sys.exit('ERROR: The detokenizer command line tools require Python 3.')
    sys.exit(main())
