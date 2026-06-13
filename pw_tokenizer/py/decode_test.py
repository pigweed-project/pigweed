#!/usr/bin/env python3
# Copyright 2022 The Pigweed Authors
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
"""Tests the tokenized string decode module."""

from datetime import datetime
import math
import unittest

import tokenized_string_decoding_test_data as tokenized_string
import varint_test_data
from pw_tokenizer import decode
from pw_tokenizer import encode


def error(msg, value=None) -> str:
    """Formats msg as the message for an argument that failed to parse."""
    if value is None:
        return '<[{}]>'.format(msg)
    return '<[{} ({})]>'.format(msg, value)


class TestDecodeTokenized(unittest.TestCase):
    """Tests decoding tokenized strings with various arguments."""

    def test_decode_generated_data(self) -> None:
        self.assertGreater(len(tokenized_string.TEST_DATA), 100)

        for fmt, decoded, encoded in tokenized_string.TEST_DATA:
            self.assertEqual(decode.decode(fmt, encoded, True), decoded)

    def test_unicode_decode_errors(self) -> None:
        """Tests unicode errors, which do not occur in the C++ decoding code."""
        self.assertEqual(
            decode.decode('Why, %c', b'\x01', True),
            'Why, ' + error('%c ERROR', -1),
        )

        self.assertEqual(
            decode.decode('%sXY%+ldxy%u', b'\x83N\x80!\x01\x02', True),
            '{}XY{}xy{}'.format(
                error('%s ERROR', "'N\\x80!'"),
                error('%+ld SKIPPED', -1),
                error('%u SKIPPED', 1),
            ),
        )

        self.assertEqual(
            decode.decode('%s%lld%9u', b'\x82$\x80\x80', True),
            '{0}{1}{2}'.format(
                error("%s ERROR ('$\\x80')"),
                error('%lld SKIPPED'),
                error('%9u SKIPPED'),
            ),
        )

        self.assertEqual(
            decode.decode('%c', b'\xff\xff\xff\xff\x0f', True),
            error('%c ERROR', -2147483648),
        )

    def test_ignore_errors(self) -> None:
        self.assertEqual(decode.decode('Why, %c', b'\x01'), 'Why, %c')

        self.assertEqual(decode.decode('%s %d', b'\x01!'), '! %d')

    def test_pointer(self) -> None:
        """Tests pointer args, which are not natively supported in Python."""
        self.assertEqual(
            decode.decode('Hello: %p', b'\x00', True), 'Hello: 0x00000000'
        )
        self.assertEqual(
            decode.decode('%p%d%d', b'\x02\x80', True),
            '0x00000001<[%d ERROR]><[%d SKIPPED]>',
        )

    def test_nothing_printed_fails(self) -> None:
        result = decode.FormatString('%n').format(b'')
        self.assertFalse(result.ok())


class TestPercentLiteralDecoding(unittest.TestCase):
    """Tests decoding the %-literal in various invalid situations."""

    def test_percent(self) -> None:
        result = decode.FormatString('%%').format(b'')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '%')
        self.assertEqual(result.remaining, b'')

    def test_percent_with_leading_plus_fails(self) -> None:
        result = decode.FormatString('%+%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_leading_negative(self) -> None:
        result = decode.FormatString('%-%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_leading_space(self) -> None:
        result = decode.FormatString('% %').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_leading_hashtag(self) -> None:
        result = decode.FormatString('%#%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_leading_zero(self) -> None:
        result = decode.FormatString('%0%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_length(self) -> None:
        """Test that all length prefixes fail to decode with %."""

        result = decode.FormatString('%hh%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%h%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%l%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ll%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%L%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%j%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%z%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%t%').format(b'')
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, b'')

    def test_percent_with_width(self):
        result = decode.FormatString('%9%').format(b'')
        self.assertFalse(result.ok())

    def test_percent_with_multidigit_width(self):
        result = decode.FormatString('%10%').format(b'')
        self.assertFalse(result.ok())

    def test_percent_with_star_width(self):
        result = decode.FormatString('%*%').format(b'')
        self.assertFalse(result.ok())

    def test_percent_with_precision(self):
        result = decode.FormatString('%.5%').format(b'')
        self.assertFalse(result.ok())

    def test_percent_with_multidigit_precision(self):
        result = decode.FormatString('%.10%').format(b'')
        self.assertFalse(result.ok())

    def test_percent_with_star_precision(self):
        result = decode.FormatString('%.*%').format(b'')
        self.assertFalse(result.ok())


# pylint: disable=too-many-public-methods
class TestIntegerDecoding(unittest.TestCase):
    """Tests decoding variable-length integers."""

    def test_signed_integer_d(self) -> None:
        result = decode.FormatString('%d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_minus(self) -> None:
        result = decode.FormatString('%-5d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10   ')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_plus(self) -> None:
        result = decode.FormatString('%+d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_blank_space(self) -> None:
        result = decode.FormatString('% d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_plus_and_blank_space_ignores_blank_space(
        self,
    ) -> None:
        result = decode.FormatString('%+ d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('% +d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_hashtag(self) -> None:
        result = decode.FormatString('%#d').format(encode.encode_args(10))
        self.assertFalse(result.ok())

    def test_signed_integer_d_with_zero(self) -> None:
        result = decode.FormatString('%05d').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '00010')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_length(self) -> None:
        """Tests that length modifiers do not affect signed integer decoding."""
        result = decode.FormatString('%hhd').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hd').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ld').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lld').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jd').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zd').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%td').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_width(self) -> None:
        result = decode.FormatString('%5d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '  -10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_d_with_width_and_0_flag(self) -> None:
        result = decode.FormatString('%05d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-0010')

    def test_signed_integer_d_with_multidigit_width(self) -> None:
        result = decode.FormatString('%10d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       -10')

    def test_signed_integer_d_with_star_width(self) -> None:
        result = decode.FormatString('%*d').format(encode.encode_args(10, -10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       -10')

    def test_signed_integer_d_with_missing_width_or_value(self) -> None:
        result = decode.FormatString('%*d').format(encode.encode_args(-10))
        self.assertFalse(result.ok())

    def test_signed_integer_d_with_precision(self) -> None:
        result = decode.FormatString('%.5d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-00010')

    def test_signed_integer_d_with_multidigit_precision(self) -> None:
        result = decode.FormatString('%.10d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-0000000010')

    def test_signed_integer_d_with_star_precision(self) -> None:
        result = decode.FormatString('%.*d').format(encode.encode_args(10, -10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-0000000010')

    def test_signed_integer_d_with_zero_precision(self) -> None:
        result = decode.FormatString('%.0d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')

    def test_signed_integer_d_with_empty_precision(self) -> None:
        result = decode.FormatString('%.d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')

    def test_zero_with_zero_precision(self) -> None:
        result = decode.FormatString('%.0d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '')

    def test_zero_with_empty_precision(self) -> None:
        result = decode.FormatString('%.d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '')

    def test_signed_integer_d_with_width_and_precision(self) -> None:
        result = decode.FormatString('%10.5d').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '    -00010')

    def test_signed_integer_d_with_star_width_and_precision(self) -> None:
        result = decode.FormatString('%*.*d').format(
            encode.encode_args(15, 10, -10)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '    -0000000010')

    def test_signed_integer_d_with_missing_precision_or_value(self) -> None:
        result = decode.FormatString('%.*d').format(encode.encode_args(-10))
        self.assertFalse(result.ok())

    def test_64_bit_specifier_workaround(self) -> None:
        result = decode.FormatString('%.u%.*lu%0*lu').format(
            encode.encode_args(0, 0, 0, 0, 0)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%.u%.*lu%0*lu').format(
            encode.encode_args(0, 0, 1, 9, 0)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '1000000000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%.u%.*lu%0*lu').format(
            encode.encode_args(1, 9, 0, 9, 0)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '1000000000000000000')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_signed_integer_i(self) -> None:
        result = decode.FormatString('%i').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_minus(self) -> None:
        result = decode.FormatString('%-5i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10   ')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_plus(self) -> None:
        result = decode.FormatString('%+i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_blank_space(self) -> None:
        result = decode.FormatString('% i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_plus_and_blank_space_ignores_blank_space(
        self,
    ) -> None:
        result = decode.FormatString('%+ i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('% +i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+10')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_hashtag(self) -> None:
        result = decode.FormatString('%#i').format(encode.encode_args(10))
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(10))

    def test_signed_integer_i_with_zero(self) -> None:
        result = decode.FormatString('%05i').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '00010')
        self.assertEqual(result.remaining, b'')

    def test_signed_integer_i_with_length(self) -> None:
        """Tests that length modifiers do not affect signed integer decoding."""
        result = decode.FormatString('%hhi').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hi').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%li').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lli').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ji').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zi').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ti').format(encode.encode_args(-10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '-10')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_unsigned_integer(self) -> None:
        result = decode.FormatString('%u').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

    def test_unsigned_integer_with_hashtag(self) -> None:
        result = decode.FormatString('%#u').format(encode.encode_args(10))
        self.assertFalse(result.ok())

    def test_unsigned_integer_with_length(self) -> None:
        """Tests that length modifiers pass unsigned integer decoding."""
        result = decode.FormatString('%hhu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ju').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lu').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '10')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_octal_integer(self) -> None:
        result = decode.FormatString('%o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_hashtag(self) -> None:
        result = decode.FormatString('%#o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_hashtag_and_width(self) -> None:
        result = decode.FormatString('%#10o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_hashtag_and_zero_and_width(self) -> None:
        result = decode.FormatString('%#010o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0000000012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_minus_and_hashtag(self) -> None:
        result = decode.FormatString('%#-5o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '012  ')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_plus_and_hashtag(self) -> None:
        result = decode.FormatString('%+#o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_space_and_hashtag(self) -> None:
        result = decode.FormatString('% #o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_zero_and_hashtag(self) -> None:
        result = decode.FormatString('%#05o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '00012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_plus_and_space_and_hashtag_ignores_space(
        self,
    ) -> None:
        result = decode.FormatString('%+ #o').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+012')
        self.assertEqual(result.remaining, b'')

    def test_octal_integer_with_length(self) -> None:
        """Tests that length modifiers do not affect octal integer decoding."""
        result = decode.FormatString('%hho').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ho').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lo').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llo').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jo').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zo').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%to').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lo').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '12')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_lowercase_hex_integer(self) -> None:
        result = decode.FormatString('%x').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_hex_integer_with_hashtag(self) -> None:
        result = decode.FormatString('%#x').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0xa')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_hex_integer_with_length(self) -> None:
        """Tests that length modifiers do not affect lowercase hex decoding."""
        result = decode.FormatString('%hhx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lx').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'a')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_uppercase_hex_integer(self) -> None:
        result = decode.FormatString('%X').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_hex_integer_with_hashtag(self) -> None:
        result = decode.FormatString('%#X').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0XA')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_hex_integer_with_length(self) -> None:
        """Tests that length modifiers do not affect uppercase hex decoding."""
        result = decode.FormatString('%hhX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%LX').format(encode.encode_args(10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'A')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_decode_generated_data(self) -> None:
        test_data = varint_test_data.TEST_DATA
        self.assertGreater(len(test_data), 100)

        for signed_spec, signed, unsigned_spec, unsigned, encoded in test_data:
            self.assertEqual(
                int(signed),
                decode.FormatSpec.from_string(signed_spec)
                .decode(bytearray(encoded))
                .value,
            )

            self.assertEqual(
                int(unsigned),
                decode.FormatSpec.from_string(unsigned_spec)
                .decode(bytearray(encoded))
                .value,
            )

    def test_zero_precision_zero_value_with_asterisks(self) -> None:
        result = decode.FormatString('%*.*d').format(b'\x00\x00\x00')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '')

        result = decode.FormatString('%*.*d').format(b'\x0a\x00\x00')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     ')

    def test_zero_precision_zero_value_with_plus_and_space(self) -> None:
        result = decode.FormatString('%+5.0d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '    +')

        result = decode.FormatString('% 5.0d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     ')

        result = decode.FormatString('%+.0d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+')

        result = decode.FormatString('% .0d').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' ')

        result = decode.FormatString('%#.0o').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0')

        result = decode.FormatString('%#5.0o').format(encode.encode_args(0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '    0')

        result = decode.FormatString('%#*o').format(encode.encode_args(-10, 10))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '012       ')


# pylint: disable=too-many-public-methods
class TestFloatDecoding(unittest.TestCase):
    """Tests decoding floating-point values using f or F."""

    def test_lowercase_float(self) -> None:
        result = decode.FormatString('%f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_minus(self) -> None:
        result = decode.FormatString('%-10f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000  ')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_plus(self) -> None:
        result = decode.FormatString('%+f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+2.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_blank_space(self) -> None:
        result = decode.FormatString('% f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 2.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_plus_and_blank_space_ignores_blank_space(
        self,
    ) -> None:
        result = decode.FormatString('%+ f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('% +f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+2.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_hashtag(self) -> None:
        result = decode.FormatString('%.0f').format(encode.encode_args(2.0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%#.0f').format(encode.encode_args(2.0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_zero(self) -> None:
        result = decode.FormatString('%010f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '002.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_length(self) -> None:
        """Tests that length modifiers do not affect f decoding."""
        result = decode.FormatString('%hhf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lf').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_width(self) -> None:
        result = decode.FormatString('%9f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 2.200000')

    def test_lowercase_float_with_multidigit_width(self) -> None:
        result = decode.FormatString('%10f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '  2.200000')

    def test_lowercase_float_with_star_width(self) -> None:
        result = decode.FormatString('%*f').format(encode.encode_args(10, 2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '  2.200000')

    def test_lowercase_float_non_number(self) -> None:
        result = decode.FormatString('%f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_with_precision(self) -> None:
        result = decode.FormatString('%.4f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2000')

    def test_lowercase_float_with_multidigit_precision(self) -> None:
        result = decode.FormatString('%.10f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2000000477')

    def test_lowercase_float_with_star_preision(self) -> None:
        result = decode.FormatString('%.*f').format(encode.encode_args(10, 2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2000000477')

    def test_lowercase_float_with_zero_precision(self) -> None:
        result = decode.FormatString('%.0f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2')

    def test_lowercase_float_with_empty_precision(self) -> None:
        result = decode.FormatString('%.f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2')

    def test_lowercase_float_with_width_and_precision(self) -> None:
        result = decode.FormatString('%10.0f').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '         2')

    def test_lowercase_float_with_star_width_and_star_precision(self) -> None:
        result = decode.FormatString('%*.*f').format(
            encode.encode_args(20, 10, 2.2)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '        2.2000000477')

    def test_lowercase_float_non_number_with_minus(self) -> None:
        result = decode.FormatString('%-5f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf  ')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_with_plus(self) -> None:
        result = decode.FormatString('%+f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_with_blank_space(self) -> None:
        result = decode.FormatString('% f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_with_plus_and_blank_ignores_blank(
        self,
    ) -> None:
        result = decode.FormatString('%+ f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+inf')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('% +f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_with_hashtag(self) -> None:
        result = decode.FormatString('%#f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_zero(self) -> None:
        result = decode.FormatString('%05f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '  inf')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_float_non_number_with_width(self) -> None:
        result = decode.FormatString('%9f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '      inf')

    def test_lowercase_float_non_number_with_multidigit_width(self) -> None:
        result = decode.FormatString('%10f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       inf')

    def test_lowercase_float_non_number_with_star_width(self) -> None:
        result = decode.FormatString('%*f').format(
            encode.encode_args(10, math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       inf')

    def test_lowercase_float_non_number_with_precision(self) -> None:
        result = decode.FormatString('%.4f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')

    def test_lowercase_float_non_number_with_multidigit_precision(self) -> None:
        result = decode.FormatString('%.10f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')

    def test_lowercase_float_non_number_with_star_preision(self) -> None:
        result = decode.FormatString('%.*f').format(
            encode.encode_args(10, math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')

    def test_lowercase_float_non_number_with_zero_precision(self) -> None:
        result = decode.FormatString('%.0f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')

    def test_lowercase_float_non_number_with_empty_precision(self) -> None:
        result = decode.FormatString('%.f').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'inf')

    def test_lowercase_float_non_number_with_width_and_precision(self) -> None:
        result = decode.FormatString('%10.0f').format(
            encode.encode_args(math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       inf')

    def test_lowercase_float_non_number_with_star_width_and_star_precision(
        self,
    ) -> None:
        result = decode.FormatString('%*.*f').format(
            encode.encode_args(10, 0, math.inf)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       inf')

    def test_zero_with_zero_precision(self) -> None:
        result = decode.FormatString('%.0f').format(encode.encode_args(0.0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0')

    def test_negative_precision(self) -> None:
        result = decode.FormatString('%*.*f').format(
            b'\x10\x03\xd0\x0f\x49\x40'
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '3.141590')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_uppercase_float(self) -> None:
        result = decode.FormatString('%F').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_float_with_length(self) -> None:
        """Tests that length modifiers do not affect F decoding."""
        result = decode.FormatString('%hhF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')

        result = decode.FormatString('%tF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%LF').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_float_non_number(self) -> None:
        result = decode.FormatString('%F').format(encode.encode_args(math.inf))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'INF')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_exponential(self) -> None:
        result = decode.FormatString('%e').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_exponential_with_length(self) -> None:
        """Tests that length modifiers do not affect e decoding."""
        result = decode.FormatString('%hhe').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        # inclusive-language: disable
        result = decode.FormatString('%he').format(encode.encode_args(2.2))
        # inclusive-language: enable
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%le').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lle').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%je').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ze').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%te').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Le').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000e+00')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_uppercase_exponential(self) -> None:
        result = decode.FormatString('%E').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_exponential_with_length(self) -> None:
        """Tests that length modifiers do not affect E decoding."""
        result = decode.FormatString('%hhE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        # inclusive-language: disable
        result = decode.FormatString('%hE').format(encode.encode_args(2.2))
        # inclusive-language: enable
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%LE').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.200000E+00')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_lowercase_shortest_take_normal(self) -> None:
        result = decode.FormatString('%g').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_shortest_take_exponential(self) -> None:
        result = decode.FormatString('%g').format(encode.encode_args(1048580.0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '1.04858e+06')
        self.assertEqual(result.remaining, b'')

    def test_lowercase_shortest_with_length(self) -> None:
        """Tests that length modifiers do not affect g decoding."""
        result = decode.FormatString('%hhg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lg').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_uppercase_shortest_take_normal(self) -> None:
        result = decode.FormatString('%G').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_shortest_take_exponential(self) -> None:
        result = decode.FormatString('%G').format(encode.encode_args(1048580.0))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '1.04858E+06')
        self.assertEqual(result.remaining, b'')

    def test_uppercase_shortest_with_length(self) -> None:
        """Tests that length modifiers do not affect G decoding."""
        result = decode.FormatString('%hhG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%LG').format(encode.encode_args(2.2))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '2.2')
        self.assertEqual(result.remaining, b'')


class TestCharDecoding(unittest.TestCase):
    """Tests decoding character values."""

    def test_char(self) -> None:
        result = decode.FormatString('%c').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

    def test_char_with_minus(self) -> None:
        result = decode.FormatString('%-5c').format(
            encode.encode_args(ord('c'))
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c    ')
        self.assertEqual(result.remaining, b'')

    def test_char_with_plus(self) -> None:
        result = decode.FormatString('%+c').format(encode.encode_args(ord('c')))
        self.assertFalse(result.ok())

    def test_char_with_blank_space(self) -> None:
        result = decode.FormatString('% c').format(encode.encode_args(ord('c')))
        self.assertFalse(result.ok())

    def test_char_with_hashtag(self) -> None:
        result = decode.FormatString('%#c').format(encode.encode_args(ord('c')))
        self.assertFalse(result.ok())

    def test_char_with_zero(self) -> None:
        result = decode.FormatString('%0c').format(encode.encode_args(ord('c')))
        self.assertFalse(result.ok())

    def test_char_with_length(self) -> None:
        """Tests that length modifiers do not affectchar decoding."""
        result = decode.FormatString('%hhc').format(
            encode.encode_args(ord('c'))
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%llc').format(
            encode.encode_args(ord('c'))
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%jc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%tc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Lc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

    def test_char_with_width(self) -> None:
        result = decode.FormatString('%5c').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '    c')

    def test_char_with_multidigit_width(self) -> None:
        result = decode.FormatString('%10c').format(
            encode.encode_args(ord('c'))
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '         c')

    def test_char_with_star_width(self) -> None:
        result = decode.FormatString('%*c').format(
            encode.encode_args(10, ord('c'))
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '         c')

    def test_char_with_precision(self) -> None:
        result = decode.FormatString('%.4c').format(
            encode.encode_args(ord('c'))
        )
        self.assertFalse(result.ok())

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_long_char(self) -> None:
        result = decode.FormatString('%lc').format(encode.encode_args(ord('c')))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'c')
        self.assertEqual(result.remaining, b'')

    def test_long_char_with_hashtag(self) -> None:
        result = decode.FormatString('%#lc').format(
            encode.encode_args(ord('c'))
        )
        self.assertFalse(result.ok())

    def test_long_char_with_zero(self) -> None:
        result = decode.FormatString('%0lc').format(
            encode.encode_args(ord('c'))
        )
        self.assertFalse(result.ok())


class TestStringDecoding(unittest.TestCase):
    """Tests decoding string values."""

    def test_string(self) -> None:
        result = decode.FormatString('%s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

    def test_string_with_minus(self) -> None:
        result = decode.FormatString('%-6s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello ')
        self.assertEqual(result.remaining, b'')

    def test_string_with_plus(self) -> None:
        result = decode.FormatString('%+s').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())

    def test_string_with_blank_space(self) -> None:
        result = decode.FormatString('% s').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())

    def test_string_with_hashtag(self) -> None:
        result = decode.FormatString('%#s').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())

    def test_string_with_zero(self) -> None:
        result = decode.FormatString('%0s').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())

    def test_string_with_length(self) -> None:
        """Tests that length modifiers do not affect string values (s)."""
        result = decode.FormatString('%hhs').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%hs').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ls').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%lls').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%js').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%zs').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%ts').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%Ls').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

    def test_string_with_width(self) -> None:
        result = decode.FormatString('%6s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' hello')

    def test_string_with_width_does_not_pad_a_string_with_same_length(
        self,
    ) -> None:
        result = decode.FormatString('%5s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')

    def test_string_with_multidigit_width(self) -> None:
        result = decode.FormatString('%10s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     hello')

    def test_string_with_star_width(self) -> None:
        result = decode.FormatString('%*s').format(
            encode.encode_args(10, 'hello')
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     hello')

    def test_string_with_precision(self) -> None:
        result = decode.FormatString('%.3s').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hel')

    def test_string_with_multidigit_precision(self) -> None:
        result = decode.FormatString('%.10s').format(
            encode.encode_args('hello')
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')

    def test_string_with_star_precision(self) -> None:
        result = decode.FormatString('%.*s').format(
            encode.encode_args(3, 'hello')
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hel')

    def test_string_with_width_and_precision(self) -> None:
        result = decode.FormatString('%10.3s').format(
            encode.encode_args('hello')
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       hel')

    def test_string_with_star_with_and_star_precision(self) -> None:
        result = decode.FormatString('%*.*s').format(
            encode.encode_args(10, 3, 'hello')
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '       hel')

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_long_string(self) -> None:
        result = decode.FormatString('%ls').format(encode.encode_args('hello'))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')
        self.assertEqual(result.remaining, b'')

    def test_long_string_with_hashtag(self) -> None:
        result = decode.FormatString('%#ls').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args('hello'))

    def test_long_string_with_zero(self) -> None:
        result = decode.FormatString('%0ls').format(encode.encode_args('hello'))
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args('hello'))

    def test_truncated_string(self) -> None:
        result = decode.FormatString('%.*s').format(b'\x14\x85hello')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello[...]')

        result = decode.FormatString('%*.*s').format(b'\x18\x14\x85hello')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '  hello[...]')

    def test_negative_precision(self) -> None:
        result = decode.FormatString('%.*s').format(b'\x03\x05hello')
        self.assertTrue(result.ok())
        self.assertEqual(result.value, 'hello')


class TestPointerDecoding(unittest.TestCase):
    """Tests decoding pointer values."""

    def test_pointer(self) -> None:
        result = decode.FormatString('%p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_minus(self) -> None:
        result = decode.FormatString('%-12p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0xDEADBEEF  ')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_plus(self) -> None:
        result = decode.FormatString('%+p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '+0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_blank_space(self) -> None:
        result = decode.FormatString('% p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_hashtag(self) -> None:
        result = decode.FormatString('%#p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

    def test_pointer_with_zero(self) -> None:
        result = decode.FormatString('%0p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

    def test_pointer_with_length(self) -> None:
        """Tests that length modifiers do not affect decoding pointers (p)."""
        result = decode.FormatString('%hhp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%hp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%lp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%llp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%jp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%zp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%tp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

        result = decode.FormatString('%Lp').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

    def test_pointer_with_width(self) -> None:
        result = decode.FormatString('%9p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_multidigit_width(self) -> None:
        result = decode.FormatString('%11p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_star_width(self) -> None:
        result = decode.FormatString('%*p').format(
            encode.encode_args(10, 0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%*p').format(
            encode.encode_args(15, 0xDEADBEEF)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     0xDEADBEEF')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_precision(self) -> None:
        result = decode.FormatString('%.10p').format(
            encode.encode_args(0xDEADBEEF)
        )
        self.assertFalse(result.ok())
        self.assertEqual(result.remaining, encode.encode_args(0xDEADBEEF))

    # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #

    def test_pointer_0_padding(self) -> None:
        result = decode.FormatString('%p').format(
            encode.encode_args(0x00000000)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0x00000000')
        self.assertEqual(result.remaining, b'')

    def test_pointer_0_with_width(self) -> None:
        result = decode.FormatString('%9p').format(
            encode.encode_args(0x00000000)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0x00000000')
        self.assertEqual(result.remaining, b'')

    def test_pointer_0_with_multidigit_width(self) -> None:
        result = decode.FormatString('%11p').format(
            encode.encode_args(0x00000000)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, ' 0x00000000')
        self.assertEqual(result.remaining, b'')

    def test_pointer_0_with_star_width(self) -> None:
        result = decode.FormatString('%*p').format(
            encode.encode_args(10, 0x00000000)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0x00000000')
        self.assertEqual(result.remaining, b'')

        result = decode.FormatString('%*p').format(
            encode.encode_args(15, 0x00000000)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '     0x00000000')
        self.assertEqual(result.remaining, b'')

    def test_pointer_with_minus_leading_zeros(self) -> None:
        result = decode.FormatString('%-12p').format(encode.encode_args(0x1234))
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0x00001234  ')
        self.assertEqual(result.remaining, b'')

    def test_pointer_dynamic_width_negative(self) -> None:
        result = decode.FormatString('%*p').format(
            encode.encode_args(-12, 0x1234)
        )
        self.assertTrue(result.ok())
        self.assertEqual(result.value, '0x00001234  ')
        self.assertEqual(result.remaining, b'')


class TestFormattedString(unittest.TestCase):
    """Tests scoring how successfully a formatted string decoded."""

    def test_no_args(self) -> None:
        result = decode.FormatString('string').format(b'')

        self.assertTrue(result.ok())
        self.assertEqual(result.score(), (True, True, 0, 0, datetime.max))

    def test_one_arg(self) -> None:
        result = decode.FormatString('%d').format(encode.encode_args(0))

        self.assertTrue(result.ok())
        self.assertEqual(result.score(), (True, True, 0, 1, datetime.max))

    def test_missing_args(self) -> None:
        result = decode.FormatString('%p%d%d').format(b'\x02\x80')

        self.assertFalse(result.ok())
        self.assertEqual(result.score(), (False, False, -2, 3, datetime.max))
        self.assertGreater(result.score(), result.score(datetime.now()))
        self.assertGreater(
            result.score(datetime.now()), result.score(datetime.min)
        )

    def test_compare_score(self) -> None:
        all_args_ok = decode.FormatString('%d%d%d').format(
            encode.encode_args(0, 0, 0)
        )
        missing_one_arg = decode.FormatString('%d%d%d').format(
            encode.encode_args(0, 0)
        )
        missing_two_args = decode.FormatString('%d%d%d').format(
            encode.encode_args(0)
        )
        all_args_extra_data = decode.FormatString('%d%d%d').format(
            encode.encode_args(0, 0, 0, 1)
        )
        missing_one_arg_extra_data = decode.FormatString('%d%d%d').format(
            b'\0' + b'\x80' * 100
        )

        self.assertGreater(all_args_ok.score(), missing_one_arg.score())
        self.assertGreater(missing_one_arg.score(), missing_two_args.score())
        self.assertGreater(
            missing_two_args.score(), all_args_extra_data.score()
        )
        self.assertGreater(
            all_args_extra_data.score(), missing_one_arg_extra_data.score()
        )


if __name__ == '__main__':
    unittest.main()
