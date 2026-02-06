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
"""Tests for detokenize."""

import base64
import concurrent
import datetime as dt
import functools
import io
import os
from pathlib import Path
import struct
import tempfile
from typing import Any, Callable, NamedTuple
import unittest
from unittest import mock

from pw_tokenizer import database
from pw_tokenizer import detokenize
from pw_tokenizer import elf_reader
from pw_tokenizer import tokens


# This function is not part of this test. It was used to generate the binary
# strings for EMPTY_ELF and ELF_WITH_TOKENIZER_SECTIONS. It takes a path and
# returns a Python byte string suitable for copying into Python source code.
def path_to_byte_string(path: str) -> str:
    with open(path, 'rb') as fd:
        data = fd.read()

    output: list[str] = []
    indices = iter(range(len(data)))

    while True:
        line = ''

        while len(line) < 70:
            try:
                i = next(indices)
            except StopIteration:
                break

            line += repr(data[i : i + 1])[2:-1].replace("'", r'\'')

        if not line:
            return ''.join(output)

        output.append("    b'{}'\n".format(''.join(line)))


# This is an empty ELF file. It was created from the ELF file for
# tokenize_test.cc with the command:
#
#   arm-none-eabi-objcopy -S --only-section NO_SECTIONS_PLEASE <ELF> <OUTPUT>
#
# The resulting ELF was converted to a Python binary string using
# path_to_byte_string function above.
EMPTY_ELF = (
    b'\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00(\x00\x01'
    b'\x00\x00\x00\xd1\x83\x00\x084\x00\x00\x00\xe0\x00\x00\x00\x00\x04\x00\x05'
    b'4\x00 \x00\x05\x00(\x00\x02\x00\x01\x00\x01\x00\x00\x00\xd4\x00\x00\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x07\x00'
    b'\x00\x00\x00\x00\x01\x00\x01\x00\x00\x00\xd4\x00\x00\x00\x00\x00\x00\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00'
    b'\x01\x00\x01\x00\x00\x00\xd4\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x01\x00\x01\x00'
    b'\x00\x00\xd4\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    b'\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x01\x00\x01\x00\x00\x00\xd4\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    b'\x06\x00\x00\x00\x00\x00\x01\x00\x00.shstrtab\x00\x00\x00\x00\x00\x00\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01'
    b'\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xd4\x00\x00'
    b'\x00\x0b\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00'
    b'\x00\x00\x00'
)

# This is an ELF file with only the pw_tokenizer sections. It was created
# from a tokenize_test binary built for the STM32F429i Discovery board. The
# pw_tokenizer sections were extracted with this command:
#
#   arm-none-eabi-objcopy -S --only-section ".pw_tokenizer*" <ELF> <OUTPUT>
#
ELF_WITH_TOKENIZER_SECTIONS_PATH = Path(__file__).parent.joinpath(
    'example_binary_with_tokenized_strings.elf'
)
ELF_WITH_TOKENIZER_SECTIONS = ELF_WITH_TOKENIZER_SECTIONS_PATH.read_bytes()

DEFAULT_DOMAIN_TOKENS = 22

# 27 total since token 881436a0="The answer is: %s" is in two domains.
ALL_DOMAIN_TOKENS = 26 + 1

# 0x2e668cd6 is 'Jello, world!' (which is also used in database_test.py).
JELLO_WORLD_TOKEN = b'\xd6\x8c\x66\x2e'


class DetokenizeTest(unittest.TestCase):
    """Tests the detokenize.Detokenizer."""

    def test_simple(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        0xCDAB, '%02d %s %c%%', date_removed=dt.datetime.now()
                    )
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\xab\xcd\0\0\x02\x03Two\x66')), '01 Two 3%'
        )

    def test_detokenize_extra_data_is_unsuccessful(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        1, 'no args', date_removed=dt.datetime(1, 1, 1)
                    )
                ]
            )
        )

        result = detok.detokenize(b'\x01\0\0\0\x04args')
        self.assertEqual(len(result.failures), 1)
        string, args, remaining = result.failures[0]
        self.assertEqual('no args', string)
        self.assertFalse(args)
        self.assertEqual(b'\x04args', remaining)
        self.assertEqual('no args', string)
        self.assertEqual('no args', str(result))

    def test_detokenize_zero_extend_short_token_with_no_args(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [tokens.TokenizedStringEntry(0xCDAB, 'This token is 16 bits')]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\xab\xcd')), 'This token is 16 bits'
        )

    def test_detokenize_missing_data_is_unsuccessful(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        2, '%s', date_removed=dt.datetime(1, 1, 1)
                    )
                ]
            )
        )

        result = detok.detokenize(b'\x02\0\0\0')
        string, args, remaining = result.failures[0]
        self.assertEqual('%s', string)
        self.assertEqual(len(args), 1)
        self.assertEqual(b'', remaining)
        self.assertEqual(len(result.failures), 1)
        self.assertEqual('%s', str(result))

    def test_detokenize_missing_data_with_errors_is_unsuccessful(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        2, '%s', date_removed=dt.datetime(1, 1, 1)
                    )
                ]
            ),
            show_errors=True,
        )

        result = detok.detokenize(b'\x02\0\0\0')
        string, args, remaining = result.failures[0]
        self.assertIn('%s MISSING', string)
        self.assertEqual(len(args), 1)
        self.assertEqual(b'', remaining)
        self.assertEqual(len(result.failures), 1)
        self.assertIn('%s MISSING', str(result))

    def test_unparsed_data(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        1, 'no args', date_removed=dt.datetime(100, 1, 1)
                    ),
                ]
            )
        )
        result = detok.detokenize(b'\x01\0\0\0o_o')
        self.assertFalse(result.ok())
        self.assertEqual('no args', str(result))
        self.assertIn('o_o', repr(result))
        self.assertIn('decoding failed', result.error_message())

    def test_empty_db(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF))
        self.assertFalse(detok.detokenize(b'\x12\x34\0\0').ok())
        self.assertIn(
            'unknown token', detok.detokenize(b'1234').error_message()
        )
        self.assertIn('unknown token', repr(detok.detokenize(b'1234')))

        self.assertEqual(
            '$' + base64.b64encode(b'1234').decode(),
            str(detok.detokenize(b'1234')),
        )

        self.assertIsNone(detok.detokenize(b'').token)

    def test_empty_db_show_errors(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF), show_errors=True)
        self.assertFalse(detok.detokenize(b'\x12\x34\0\0').ok())
        self.assertIn(
            'unknown token', detok.detokenize(b'1234').error_message()
        )
        self.assertIn('unknown token', repr(detok.detokenize(b'1234')))
        self.assertIn('unknown token', str(detok.detokenize(b'1234')))

        self.assertIsNone(detok.detokenize(b'').token)

    def test_missing_token_show_errors(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF), show_errors=True)
        self.assertIn('missing token', detok.detokenize(b'').error_message())
        self.assertIn('missing token', str(detok.detokenize(b'')))

    def test_missing_token(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF))
        self.assertIn('missing token', detok.detokenize(b'').error_message())
        self.assertEqual('$', str(detok.detokenize(b'')))

    def test_unknown_shorter_token_show_error(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF), show_errors=True)

        self.assertIn('unknown token', detok.detokenize(b'1').error_message())
        self.assertIn('unknown token', str(detok.detokenize(b'1')))
        self.assertIn('unknown token', repr(detok.detokenize(b'1')))

        self.assertIn('unknown token', detok.detokenize(b'123').error_message())
        self.assertIn('unknown token', str(detok.detokenize(b'123')))
        self.assertIn('unknown token', repr(detok.detokenize(b'123')))

    def test_unknown_shorter_token(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(EMPTY_ELF))

        self.assertEqual(
            'unknown token 00000001', detok.detokenize(b'\1').error_message()
        )
        self.assertEqual(
            '$' + base64.b64encode(b'\1\0\0\0').decode(),
            str(detok.detokenize(b'\1')),
        )
        self.assertIn('unknown token 00000001', repr(detok.detokenize(b'\1')))

        self.assertEqual(
            'unknown token 00030201',
            detok.detokenize(b'\1\2\3').error_message(),
        )
        self.assertEqual(
            '$' + base64.b64encode(b'\1\2\3\0').decode(),
            str(detok.detokenize(b'\1\2\3')),
        )
        self.assertIn(
            'unknown token 00030201', repr(detok.detokenize(b'\1\2\3'))
        )

    def test_decode_from_elf_data(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS))

        self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())
        self.assertEqual(
            str(detok.detokenize(JELLO_WORLD_TOKEN)), 'Jello, world!'
        )

        undecoded_args = detok.detokenize(JELLO_WORLD_TOKEN + b'some junk')
        self.assertFalse(undecoded_args.ok())
        self.assertEqual(str(undecoded_args), 'Jello, world!')

        self.assertTrue(detok.detokenize(b'\0\0\0\0').ok())
        self.assertEqual(str(detok.detokenize(b'\0\0\0\0')), '')

    def test_decode_from_elf_file(self) -> None:
        """Test decoding from an elf file."""
        detok = detokenize.Detokenizer(io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS))
        expected_tokens = frozenset(detok.database.token_to_entries.keys())

        with tempfile.NamedTemporaryFile('wb', delete=False) as elf:
            try:
                elf.write(ELF_WITH_TOKENIZER_SECTIONS)
                elf.close()

                # Open ELF by file object
                with open(elf.name, 'rb') as fd:
                    detok = detokenize.Detokenizer(fd)

                self.assertEqual(
                    expected_tokens,
                    frozenset(detok.database.token_to_entries.keys()),
                )

                # Open ELF by path
                detok = detokenize.Detokenizer(elf.name)
                self.assertEqual(
                    expected_tokens,
                    frozenset(detok.database.token_to_entries.keys()),
                )

                # Open ELF by elf_reader.Elf
                with open(elf.name, 'rb') as fd:
                    detok = detokenize.Detokenizer(elf_reader.Elf(fd))

                self.assertEqual(
                    expected_tokens,
                    frozenset(detok.database.token_to_entries.keys()),
                )
            finally:
                os.unlink(elf.name)

    def test_decode_from_csv_file(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS))
        expected_tokens = frozenset(detok.database.token_to_entries.keys())

        csv_database = str(detok.database)
        self.assertEqual(len(csv_database.splitlines()), ALL_DOMAIN_TOKENS)

        with tempfile.NamedTemporaryFile('w', delete=False) as csv_file:
            try:
                csv_file.write(csv_database)
                csv_file.close()

                # Open CSV by path
                detok = detokenize.Detokenizer(csv_file.name)
                self.assertEqual(
                    expected_tokens,
                    frozenset(detok.database.token_to_entries.keys()),
                )

                # Open CSV by file object
                with open(csv_file.name) as fd:
                    detok = detokenize.Detokenizer(fd)

                self.assertEqual(
                    expected_tokens,
                    frozenset(detok.database.token_to_entries.keys()),
                )
            finally:
                os.unlink(csv_file.name)

    def test_create_detokenizer_with_token_database(self) -> None:
        detok = detokenize.Detokenizer(io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS))
        expected_tokens = frozenset(detok.database.token_to_entries.keys())

        detok = detokenize.Detokenizer(detok.database)
        self.assertEqual(
            expected_tokens, frozenset(detok.database.token_to_entries.keys())
        )


class DetokenizeWithCollisions(unittest.TestCase):
    """Tests collision resolution."""

    def setUp(self) -> None:
        super().setUp()
        token = 0xBAAD

        # Database with several conflicting tokens.
        self.detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        token, 'REMOVED', date_removed=dt.datetime(9, 1, 1)
                    ),
                    tokens.TokenizedStringEntry(token, 'newer'),
                    tokens.TokenizedStringEntry(
                        token, 'A: %d', date_removed=dt.datetime(30, 5, 9)
                    ),
                    tokens.TokenizedStringEntry(
                        token, 'B: %c', date_removed=dt.datetime(30, 5, 10)
                    ),
                    tokens.TokenizedStringEntry(token, 'C: %s'),
                    tokens.TokenizedStringEntry(token, '%d%u'),
                    tokens.TokenizedStringEntry(token, '%s%u %d'),
                    tokens.TokenizedStringEntry(1, '%s'),
                    tokens.TokenizedStringEntry(1, '%d'),
                    tokens.TokenizedStringEntry(2, 'Three %s %s %s'),
                    tokens.TokenizedStringEntry(2, 'Five %d %d %d %d %s'),
                ]
            )
        )

    def test_collision_no_args_favors_most_recently_present(self) -> None:
        no_args = self.detok.detokenize(b'\xad\xba\0\0')
        self.assertFalse(no_args.ok())
        self.assertEqual(len(no_args.successes), 2)
        self.assertEqual(len(no_args.failures), 5)
        self.assertEqual(len(no_args.matches()), 7)
        self.assertEqual(str(no_args), 'newer')
        best_result = no_args.best_result()
        assert best_result is not None
        self.assertEqual(len(best_result.args), 0)
        self.assertEqual(best_result[0], 'newer')

    def test_collision_one_integer_arg_favors_most_recently_present(
        self,
    ) -> None:
        multiple_correct = self.detok.detokenize(b'\xad\xba\0\0\x7a')
        self.assertFalse(multiple_correct.ok())
        self.assertIn('ERROR', repr(multiple_correct))
        self.assertEqual(len(multiple_correct.successes), 2)
        self.assertEqual(len(multiple_correct.failures), 5)
        self.assertEqual(len(multiple_correct.matches()), 7)
        self.assertEqual(str(multiple_correct), 'B: =')

    def test_collision_one_integer_arg_favor_successful_decode(self) -> None:
        # One string decodes successfully, since the arg is out of range for %c.
        int_arg = self.detok.detokenize(b'\xad\xba\0\0\xfe\xff\xff\xff\x0f')
        self.assertTrue(int_arg.ok())
        self.assertEqual(str(int_arg), 'A: 2147483647')

    def test_collision_one_string_arg_favors_successful_decode(self) -> None:
        # One string decodes successfully, since decoding the argument as an
        # integer does not decode all the data.
        string_arg = self.detok.detokenize(b'\xad\xba\0\0\x02Hi')
        self.assertTrue(string_arg.ok())
        self.assertEqual(str(string_arg), 'C: Hi')

    def test_collision_one_string_arg_favors_decoding_all_data(self) -> None:
        result = self.detok.detokenize(b'\1\0\0\0\x83hi')
        self.assertEqual(len(result.failures), 2)
        # Should resolve to the string since %d would leave one byte behind.
        self.assertEqual(str(result), '%s')

    def test_collision_multiple_args_favors_decoding_more_arguments(
        self,
    ) -> None:
        result = self.detok.detokenize(b'\2\0\0\0\1\2\1\4\5')
        self.assertEqual(len(result.matches()), 2)
        self.assertEqual(result.matches()[0][0], 'Five -1 1 -1 2 %s')
        self.assertEqual(result.matches()[1][0], 'Three \2 \4 %s')

    def test_collision_multiple_args_favors_decoding_all_arguments(
        self,
    ) -> None:
        unambiguous = self.detok.detokenize(b'\xad\xba\0\0\x01#\x00\x01')
        self.assertTrue(unambiguous.ok())
        self.assertEqual(len(unambiguous.matches()), 7)
        self.assertEqual('#0 -1', str(unambiguous))
        self.assertIn('#0 -1', repr(unambiguous))


class ManualPoolExecutor(concurrent.futures.Executor):
    """A stubbed pool executor that captures the most recent work request
    and holds it until the public process method is manually called."""

    def __init__(self) -> None:
        super().__init__()
        self._func = None

    # pylint: disable=arguments-differ
    def submit(self, func, *args, **kwargs):
        """Submits work to the pool, stashing the partial for later use."""
        self._func = functools.partial(func, *args, **kwargs)

    def process(self) -> None:
        """Processes the latest func submitted to the pool."""
        if self._func is not None:
            self._func()
            self._func = None


class InlinePoolExecutor(concurrent.futures.Executor):
    """A stubbed pool executor that runs work immediately, inline."""

    # pylint: disable=arguments-differ
    def submit(self, func, *args, **kwargs):
        """Submits work to the pool, stashing the partial for later use."""
        func(*args, **kwargs)


@mock.patch('os.path.getmtime')
class AutoUpdatingDetokenizerTest(unittest.TestCase):
    """Tests the AutoUpdatingDetokenizer class."""

    def test_update(self, mock_getmtime) -> None:
        """Tests the update command."""

        db = database.load_token_database(
            io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS)
        )
        self.assertEqual(len(db), ALL_DOMAIN_TOKENS)

        the_time = [100]

        def move_back_time_if_file_exists(path) -> int:
            if os.path.exists(path):
                the_time[0] -= 1
                return the_time[0]

            raise FileNotFoundError

        mock_getmtime.side_effect = move_back_time_if_file_exists

        with tempfile.NamedTemporaryFile('wb', delete=False) as file:
            try:
                file.close()

                pool = ManualPoolExecutor()
                detok = detokenize.AutoUpdatingDetokenizer(
                    file.name, min_poll_period_s=0, pool=pool
                )
                self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                with open(file.name, 'wb') as fd:
                    tokens.write_binary(db, fd)

                # After the change but before the pool runs in another thread,
                # the token should not exist.
                self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                # After the pool is allowed to process, it should.
                pool.process()
                self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())
            finally:
                os.unlink(file.name)

    def test_update_with_directory(self, mock_getmtime) -> None:
        """Tests the update command with a directory format database."""
        db = database.load_token_database(
            io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS)
        )
        self.assertEqual(len(db), ALL_DOMAIN_TOKENS)

        the_time = [100]

        def move_back_time_if_file_exists(path: str) -> int:
            if os.path.exists(path):
                the_time[0] -= 1
                return the_time[0]

            raise FileNotFoundError

        mock_getmtime.side_effect = move_back_time_if_file_exists

        with tempfile.TemporaryDirectory() as dbdir:
            with (
                tempfile.NamedTemporaryFile(
                    'wb', delete=False, suffix='.pw_tokenizer.csv', dir=dbdir
                ) as matching_suffix_file,
                tempfile.NamedTemporaryFile(
                    'wb', delete=False, suffix='.not.right', dir=dbdir
                ) as mismatched_suffix_file,
            ):
                try:
                    matching_suffix_file.close()
                    mismatched_suffix_file.close()

                    pool = ManualPoolExecutor()
                    detok = detokenize.AutoUpdatingDetokenizer(
                        dbdir, min_poll_period_s=0, pool=pool
                    )
                    self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                    with open(mismatched_suffix_file.name, 'wb') as fd:
                        tokens.write_csv(db, fd)
                    pool.process()
                    self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                    with open(matching_suffix_file.name, 'wb') as fd:
                        tokens.write_csv(db, fd)

                    # After the change but before the pool runs in another
                    # thread, the token should not exist.
                    self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())
                    pool.process()

                    # After the pool is allowed to process, it should.
                    self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())
                finally:
                    os.unlink(mismatched_suffix_file.name)
                    os.unlink(matching_suffix_file.name)
                    os.rmdir(dbdir)

        # The database stays around if the file is deleted.
        self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())

    def test_no_update_if_time_is_same(self, mock_getmtime) -> None:
        mock_getmtime.return_value = 100

        with tempfile.NamedTemporaryFile('wb', delete=False) as file:
            try:
                tokens.write_csv(
                    database.load_token_database(
                        io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS)
                    ),
                    file,
                )
                file.close()

                detok = detokenize.AutoUpdatingDetokenizer(
                    file.name, min_poll_period_s=0, pool=InlinePoolExecutor()
                )
                self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                # Empty the database, but keep the mock modified time the same.
                with open(file.name, 'wb'):
                    pass

                self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())
                self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())

                # Move back time so the now-empty file is reloaded.
                mock_getmtime.return_value = 50
                self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())
            finally:
                os.unlink(file.name)

    def test_token_domain_in_str(self, _) -> None:
        """Tests a str containing a domain."""
        detok = detokenize.AutoUpdatingDetokenizer(
            f'{ELF_WITH_TOKENIZER_SECTIONS_PATH}#',  # Default domain
            min_poll_period_s=0,
            pool=InlinePoolExecutor(),
        )
        self.assertEqual(len(detok.database), DEFAULT_DOMAIN_TOKENS)

    def test_token_domain_in_path(self, _) -> None:
        """Tests a Path() containing a domain."""
        detok = detokenize.AutoUpdatingDetokenizer(
            Path(f'{ELF_WITH_TOKENIZER_SECTIONS_PATH}#'),
            min_poll_period_s=0,
            pool=InlinePoolExecutor(),
        )
        self.assertEqual(len(detok.database), DEFAULT_DOMAIN_TOKENS)

    def test_token_no_domain_in_str(self, _) -> None:
        """Tests a str without a domain, which loads all domains."""
        detok = detokenize.AutoUpdatingDetokenizer(
            str(ELF_WITH_TOKENIZER_SECTIONS_PATH),
            min_poll_period_s=0,
            pool=InlinePoolExecutor(),
        )
        self.assertEqual(len(detok.database), ALL_DOMAIN_TOKENS)

    def test_token_no_domain_in_path(self, _) -> None:
        """Tests a Path() without a domain, which loads all domains."""
        detok = detokenize.AutoUpdatingDetokenizer(
            ELF_WITH_TOKENIZER_SECTIONS_PATH,
            min_poll_period_s=0,
            pool=InlinePoolExecutor(),
        )
        self.assertEqual(len(detok.database), ALL_DOMAIN_TOKENS)

    def test_invalid_domain_specification(self, _) -> None:
        with self.assertRaises(ValueError, msg='Too many # delimiters'):
            detokenize.AutoUpdatingDetokenizer(
                f'{ELF_WITH_TOKENIZER_SECTIONS_PATH}##',
                min_poll_period_s=0,
                pool=InlinePoolExecutor(),
            )

    def test_extra_databases(self, mock_getmtime) -> None:
        """Tests that extra_databases are merged and survive reloads."""
        db = database.load_token_database(
            io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS)
        )
        extra_token = 0x1234
        extra_db = tokens.Database(
            [tokens.TokenizedStringEntry(extra_token, 'extra token')]
        )

        the_time = [100]
        mock_getmtime.side_effect = lambda _: the_time[0]

        with tempfile.NamedTemporaryFile('wb', delete=False) as file:
            try:
                tokens.write_binary(db, file.file)  # type: ignore[arg-type]
                file.close()

                pool = InlinePoolExecutor()
                detok = detokenize.AutoUpdatingDetokenizer(
                    file.name,
                    min_poll_period_s=0,
                    pool=pool,
                    extra_databases=[extra_db],
                )

                # Both the file tokens and the extra token should be present.
                self.assertTrue(detok.detokenize(JELLO_WORLD_TOKEN).ok())
                self.assertTrue(
                    detok.detokenize(struct.pack('<I', extra_token)).ok()
                )
                self.assertEqual(
                    str(detok.detokenize(struct.pack('<I', extra_token))),
                    'extra token',
                )

                # Update the file and trigger a reload.
                the_time[0] += 1
                with open(file.name, 'wb') as fd:
                    # Write an empty database to the file.
                    tokens.write_binary(tokens.Database(), fd)

                # The file token should be gone, but the extra token should
                # remain.
                self.assertFalse(detok.detokenize(JELLO_WORLD_TOKEN).ok())
                self.assertTrue(
                    detok.detokenize(struct.pack('<I', extra_token)).ok()
                )
                self.assertEqual(
                    str(detok.detokenize(struct.pack('<I', extra_token))),
                    'extra token',
                )

            finally:
                os.unlink(file.name)

    def test_extra_databases_multiple_and_iterable(self, mock_getmtime) -> None:
        """Tests that multiple extra_databases and iterables are supported."""
        db1 = tokens.Database([tokens.TokenizedStringEntry(0x1, 'token 1')])
        db2 = tokens.Database([tokens.TokenizedStringEntry(0x2, 'token 2')])

        mock_getmtime.return_value = 100

        # Test with a generator of multiple databases
        def db_gen():
            yield db1
            yield db2

        detok = detokenize.AutoUpdatingDetokenizer(
            extra_databases=db_gen(),
            min_poll_period_s=0,
            pool=InlinePoolExecutor(),
        )

        self.assertTrue(detok.detokenize(struct.pack('<I', 0x1)).ok())
        self.assertEqual(
            str(detok.detokenize(struct.pack('<I', 0x1))), 'token 1'
        )
        self.assertTrue(detok.detokenize(struct.pack('<I', 0x2)).ok())
        self.assertEqual(
            str(detok.detokenize(struct.pack('<I', 0x2))), 'token 2'
        )


def _next_char(message: bytes) -> bytes:
    return bytes(b + 1 for b in message)


class NestedMessageParserTest(unittest.TestCase):
    """Tests parsing prefixed messages."""

    class _Case(NamedTuple):
        data: bytes
        expected: bytes
        title: str
        transform: Callable[[bytes], bytes] = _next_char

    TRANSFORM_TEST_CASES = (
        _Case(b'$abcd', b'%bcde', 'single message'),
        _Case(
            b'$$WHAT?$abc$WHY? is this $ok $',
            b'%%WHAT?%bcd%WHY? is this %ok %',
            'message and non-message',
        ),
        _Case(b'$1$', b'%1%', 'empty message'),
        _Case(b'$abc$defgh', b'%bcd%efghh', 'sequential message'),
        _Case(
            b'w$abcx$defygh$$abz',
            b'w$ABCx$DEFygh$$ABz',
            'interspersed start/end non-message',
            bytes.upper,
        ),
        _Case(
            b'$abcx$defygh$$ab',
            b'$ABCx$DEFygh$$AB',
            'interspersed start/end message ',
            bytes.upper,
        ),
    )

    def setUp(self) -> None:
        self.decoder = detokenize.NestedMessageParser('$', 'abcdefg')

    def test_transform_io(self) -> None:
        for data, expected, title, transform in self.TRANSFORM_TEST_CASES:
            self.assertEqual(
                expected,
                b''.join(
                    self.decoder.transform_io(io.BytesIO(data), transform)
                ),
                f'{title}: {data!r}',
            )

    def test_transform_bytes_with_flush(self) -> None:
        for data, expected, title, transform in self.TRANSFORM_TEST_CASES:
            self.assertEqual(
                expected,
                self.decoder.transform(data, transform, flush=True),
                f'{title}: {data!r}',
            )

    def test_transform_bytes_sequential(self) -> None:
        def transform(message):
            return message.upper().replace(b'$', b'*')

        self.assertEqual(self.decoder.transform(b'abc$abcd', transform), b'abc')
        self.assertEqual(self.decoder.transform(b'$', transform), b'*ABCD')
        self.assertEqual(self.decoder.transform(b'$b', transform), b'*')
        self.assertEqual(self.decoder.transform(b'', transform), b'')
        self.assertEqual(self.decoder.transform(b' ', transform), b'*B ')
        self.assertEqual(self.decoder.transform(b'hello', transform), b'hello')
        self.assertEqual(self.decoder.transform(b'?? $ab', transform), b'?? ')
        self.assertEqual(
            self.decoder.transform(b'123$ab4$56$a', transform), b'*AB123*AB4*56'
        )
        self.assertEqual(
            self.decoder.transform(b'bc', transform, flush=True), b'*ABC'
        )

    MESSAGES_TEST: Any = (
        (b'123$abc456$a', (False, b'123'), (True, b'$abc'), (False, b'456')),
        (b'7$abcd', (True, b'$a'), (False, b'7')),
        (b'e',),
        (b'',),
        (b'$', (True, b'$abcde')),
        (b'$', (True, b'$')),
        (b'$a$b$c', (True, b'$'), (True, b'$a'), (True, b'$b')),
        (b'1', (True, b'$c'), (False, b'1')),
        (b'',),
        (b'?', (False, b'?')),
        (b'!@', (False, b'!@')),
        (b'%^&', (False, b'%^&')),
    )

    def test_read_messages(self) -> None:
        for step in self.MESSAGES_TEST:
            data: bytes = step[0]
            pieces: tuple[tuple[bool, bytes], ...] = step[1:]
            self.assertEqual(tuple(self.decoder.read_messages(data)), pieces)

    def test_read_messages_flush(self) -> None:
        self.assertEqual(
            list(self.decoder.read_messages(b'123$a')), [(False, b'123')]
        )
        self.assertEqual(list(self.decoder.read_messages(b'b')), [])
        self.assertEqual(
            list(self.decoder.read_messages(b'', flush=True)), [(True, b'$ab')]
        )

    def test_read_messages_io(self) -> None:
        # Rework the read_messages test data for stream input.
        data = io.BytesIO(b''.join(step[0] for step in self.MESSAGES_TEST))
        expected_pieces = sum((step[1:] for step in self.MESSAGES_TEST), ())

        result = self.decoder.read_messages_io(data)
        for expected_is_message, expected_data in expected_pieces:
            if expected_is_message:
                is_message, piece = next(result)
                self.assertTrue(is_message)
                self.assertEqual(expected_data, piece)
            else:  # the IO version yields non-messages byte by byte
                for byte in expected_data:
                    is_message, piece = next(result)
                    self.assertFalse(is_message)
                    self.assertEqual(bytes([byte]), piece)


class DetokenizeNested(unittest.TestCase):
    """Tests detokenizing nested tokens"""

    def test_nested_hashed_arg(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'tokenized argument'),
                    tokens.TokenizedStringEntry(
                        2,
                        'This is a ' + '$#%08x',
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),
            'This is a tokenized argument',
        )

    def test_nested_base64_arg(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(1, 'base64 argument'),
                    tokens.TokenizedStringEntry(2, 'This is a %s'),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x09$AQAAAA==')),  # token for 1
            'This is a base64 argument',
        )

    def test_deeply_nested_arg(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(1, '$10#0000000005'),
                    tokens.TokenizedStringEntry(2, 'This is a $#%08x'),
                    tokens.TokenizedStringEntry(3, 'deeply nested argument'),
                    tokens.TokenizedStringEntry(4, '$AQAAAA=='),
                    tokens.TokenizedStringEntry(5, '$AwAAAA=='),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x08')),  # token for 4
            'This is a deeply nested argument',
        )


class DetokenizeNestedMessages(unittest.TestCase):
    """Tests detokenizing nested messages."""

    JELLO = b'$' + base64.b64encode(JELLO_WORLD_TOKEN)

    RECURSION_STRING = f'The secret message is "{JELLO.decode()}"'
    RECURSION = b'$' + base64.b64encode(
        struct.pack('I', tokens.c_hash(RECURSION_STRING))
    )

    RECURSION_STRING_2 = f"'{RECURSION.decode()}', said the spy."
    RECURSION_2 = b'$' + base64.b64encode(
        struct.pack('I', tokens.c_hash(RECURSION_STRING_2))
    )

    TEST_CASES = (
        (b'', b''),
        (b'nothing here', b'nothing here'),
        (JELLO, b'Jello, world!'),
        (JELLO + b'a', b'Jello, world!a'),
        (JELLO + b'abc', b'Jello, world!abc'),
        (JELLO + b'abc=', b'Jello, world!abc='),
        (b'$a' + JELLO + b'a', b'$aJello, world!a'),
        (b'Hello ' + JELLO + b'?', b'Hello Jello, world!?'),
        (b'$' + JELLO, b'$Jello, world!'),
        (JELLO + JELLO, b'Jello, world!Jello, world!'),
        (JELLO + b'$' + JELLO, b'Jello, world!$Jello, world!'),
        (JELLO + b'$a' + JELLO + b'bcd', b'Jello, world!$aJello, world!bcd'),
        (b'$3141', b'$3141'),
        (JELLO + b'$3141', b'Jello, world!$3141'),
        (
            JELLO + b'$a' + JELLO + b'b' + JELLO + b'c',
            b'Jello, world!$aJello, world!bJello, world!c',
        ),
        (RECURSION, b'The secret message is "Jello, world!"'),
        (
            RECURSION_2,
            b'\'The secret message is "Jello, world!"\', said the spy.',
        ),
    )

    def setUp(self) -> None:
        super().setUp()
        db = database.load_token_database(
            io.BytesIO(ELF_WITH_TOKENIZER_SECTIONS)
        )
        db.add(
            tokens.TokenizedStringEntry(tokens.c_hash(s), s)
            for s in [self.RECURSION_STRING, self.RECURSION_STRING_2]
        )
        self.detok = detokenize.Detokenizer(db)

    def test_detokenize_text_live(self) -> None:
        for data, expected in self.TEST_CASES:
            output = io.BytesIO()
            self.detok.detokenize_text_live(io.BytesIO(data), output)

            self.assertEqual(expected, output.getvalue(), f'Input: {data!r}')

    def test_detokenize_text_to_file(self) -> None:
        for data, expected in self.TEST_CASES:
            output = io.BytesIO()
            self.detok.detokenize_text_to_file(data, output)

            self.assertEqual(expected, output.getvalue())

    def test_detokenize_text(self) -> None:
        for data, expected in self.TEST_CASES:
            self.assertEqual(expected, self.detok.detokenize_text(data))

    def test_detokenize_text_str(self) -> None:
        for data, expected in self.TEST_CASES:
            self.assertEqual(
                expected.decode(), self.detok.detokenize_text(data.decode())
            )


class DetokenizeInfiniteRecursion(unittest.TestCase):
    """Tests that infinite nested token recursion resolves."""

    def setUp(self) -> None:
        super().setUp()
        self.detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0, '$AAAAAA=='),  # token for 0
                    tokens.TokenizedStringEntry(1, '$AgAAAA=='),  # token for 2
                    tokens.TokenizedStringEntry(2, '$#00000003'),  # token for 3
                    tokens.TokenizedStringEntry(3, '$AgAAAA=='),  # token for 2
                ]
            )
        )

    def test_detokenize_self_recursion(self) -> None:
        for depth in range(5):
            self.assertEqual(
                self.detok.detokenize_text(
                    b'This one is deep: $AAAAAA==', recursion=depth
                ),
                b'This one is deep: $AAAAAA==',
            )

    def test_detokenize_self_recursion_default(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(
                b'This one is deep: $AAAAAA==',
            ),
            b'This one is deep: $AAAAAA==',
        )

    def test_detokenize_cyclic_recursion_even(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(b'I said "$AQAAAA=="', recursion=6),
            b'I said "$AgAAAA=="',
        )

    def test_detokenize_cyclic_recursion_odd(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(b'I said "$AQAAAA=="', recursion=7),
            b'I said "$#00000003"',
        )


class DetokenizeNestedfiniteRecursion(unittest.TestCase):
    """Tests that infinite Bas64 token recursion resolves."""

    def setUp(self) -> None:
        super().setUp()
        self.detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0, '$AAAAAA=='),  # token for 0
                    tokens.TokenizedStringEntry(1, '$AgAAAA=='),  # token for 2
                    tokens.TokenizedStringEntry(2, '$AwAAAA=='),  # token for 3
                    tokens.TokenizedStringEntry(3, '$AgAAAA=='),  # token for 2
                ]
            )
        )

    def test_detokenize_self_recursion(self) -> None:
        for depth in range(5):
            self.assertEqual(
                self.detok.detokenize_text(
                    b'This one is deep: $AAAAAA==', recursion=depth
                ),
                b'This one is deep: $AAAAAA==',
            )

    def test_detokenize_self_recursion_default(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(b'This one is deep: $64#AAAAAA=='),
            b'This one is deep: $AAAAAA==',
        )

    def test_detokenize_cyclic_recursion_even(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(b'I said "$AQAAAA=="', recursion=2),
            b'I said "$AgAAAA=="',
        )

    def test_detokenize_cyclic_recursion_odd(self) -> None:
        self.assertEqual(
            self.detok.detokenize_text(b'I said "$AQAAAA=="', recursion=3),
            b'I said "$AwAAAA=="',
        )


class DetokenizeNestedDomains(unittest.TestCase):
    """Tests detokenizing nested tokens with specified domains"""

    def test_nested_hashed_arg_with_one_domain_match(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'domain1', 'D1'),
                    tokens.TokenizedStringEntry(
                        2, 'This is all in ' + '${D1}#%08x', 'D1'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),
            'This is all in domain1',
        )

    def test_multiple_nested_args_in_one_sentence(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'nested token 1', 'D1'),
                    tokens.TokenizedStringEntry(
                        2,
                        'This is '
                        + '${D1}#%08x'
                        + ' and this is '
                        + '${D1}#00000003',
                        'D1',
                    ),
                    tokens.TokenizedStringEntry(3, 'nested token 2', 'D1'),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),
            'This is nested token 1 and this is nested token 2',
        )

    def test_nested_hashed_arg_with_two_domain_match(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        0xA, 'and this is domain1', 'D1'
                    ),
                    tokens.TokenizedStringEntry(
                        2, 'This is domain2 ' + '${D1}#%08x', 'D2'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),
            'This is domain2 and this is domain1',
        )

    def test_nested_hashed_arg_with_different_domains(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'domain1', 'D1'),
                    tokens.TokenizedStringEntry(
                        2, 'This is all in ' + '${D2}#%08x', 'D1'
                    ),
                ]
            )
        )
        result = detok.detokenize(b'\x02\0\0\0\x14')
        self.assertFalse(result == 'This is all in domain1')

    def test_nested_messages_arg_multiple_domains(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(1, '${D5}#00000004', 'D1'),
                    tokens.TokenizedStringEntry(2, 'This is a %s', 'D1'),
                    tokens.TokenizedStringEntry(3, 'base64 argument', 'D2'),
                    tokens.TokenizedStringEntry(
                        4, 'nested ' + '${D2}#00000003', 'D5'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x09$AQAAAA==')),  # token for 1
            'This is a $AQAAAA==',
            'Nested decoding fails no domain was specified, but token is in D1',
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x0D${D1}AQAAAA==')),
            'This is a nested base64 argument',
            'Succeeds when domain is specified',
        )

    def test_nested_hashed_arg_with_domain_whitespace(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(
                        0xA, 'and this is domain1', 'Domain1'
                    ),
                    tokens.TokenizedStringEntry(
                        2, 'This is domain2 ' + '${Domain 1}#%08x', 'D2'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),
            'This is domain2 and this is domain1',
        )

    def test_double_nested_messages_arg_multiple_domains(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(1, 'Ignore this', 'D1'),
                    tokens.TokenizedStringEntry(
                        2, 'This is a ${${D2}#00000004}#00000003', 'D1'
                    ),
                    tokens.TokenizedStringEntry(
                        3, 'double nested base64 argument', 'D3'
                    ),
                    tokens.TokenizedStringEntry(4, 'D3', 'D2'),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),  # token for 2
            'This is a double nested base64 argument',
        )

    def test_empty_domain(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'an empty domain!', ''),
                    tokens.TokenizedStringEntry(
                        2, 'This argument is in ' + '${}#%08x', 'D1'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),  # token for 2
            'This argument is in an empty domain!',
        )

    def test_whitespace_before_domain(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(0xA, 'domain 2', 'D2'),
                    tokens.TokenizedStringEntry(
                        2, 'This argument is in ' + '${      D2}#%08x', 'D1'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),  # token for 2
            'This argument is in domain 2',
        )

    def test_nested_token_without_domain(self) -> None:
        detok = detokenize.Detokenizer(
            tokens.Database(
                [
                    tokens.TokenizedStringEntry(1, 'D3', ''),
                    tokens.TokenizedStringEntry(
                        2, '${$AQAAAA==}#00000003', 'D1'
                    ),
                    tokens.TokenizedStringEntry(
                        3, 'This is a double-nested token :)', 'D3'
                    ),
                ]
            )
        )
        self.assertEqual(
            str(detok.detokenize(b'\x02\0\0\0\x14')),  # token for 2
            'This is a double-nested token :)',
        )


if __name__ == '__main__':
    unittest.main()
