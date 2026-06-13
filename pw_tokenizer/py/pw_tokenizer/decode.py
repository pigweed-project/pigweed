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
"""Decodes arguments and formats tokenized messages.

The decode(format_string, encoded_arguments) function provides a simple way to
format a string with encoded arguments. The FormatString class may also be used.

Missing, truncated, or otherwise corrupted arguments are handled and displayed
in the resulting string with an error message.
"""

from __future__ import annotations

from datetime import datetime
import math
import re
import struct
from typing import (
    Any,
    Iterable,
    NamedTuple,
    Match,
    Sequence,
)


def zigzag_decode(value: int) -> int:
    """ZigZag decode function from protobuf's wire_format module."""
    if not value & 0x1:
        return value >> 1
    return (value >> 1) ^ (~0)


class FormatSpec:
    """Represents a format specifier parsed from a printf-style string.

    This implementation is designed to align with the C99 specification,
    section 7.19.6
    (https://www.dii.uchile.cl/~daespino/files/Iso_C_1999_definition.pdf).
    Notably, this specification is slightly different than what is implemented
    in most compilers due to each compiler choosing to interpret undefined
    behavior in slightly different ways. Treat the following description as the
    source of truth.

    This implementation supports:
    - Overall Format: `%[flags][width][.precision][length][specifier]`
    - Flags (Zero or More)
      - `-`: Left-justify within the given field width; Right justification is
             the default (see Width modifier).
      - `+`: Forces to preceed the result with a plus or minus sign (`+` or `-`)
             even for positive numbers. By default, only negative numbers are
             preceded with a `-` sign.
      - ` ` (space): If no sign is going to be written, a blank space is
                     inserted before the value.
      - `#`: Specifies an alternative print syntax should be used.
        - Used with `o`, `x` or `X` specifiers the value is preceeded with `0`,
          `0x`, or `0X`, respectively, for values different than zero.
        - Used with `a`, `A`, `e`, `E`, `f`, `F`, `g`, or `G` it forces the
          written output to contain a decimal point even if no more digits
          follow. By default, if no digits follow, no decimal point is written.
      - `0`: Left-pads the number with zeroes (`0`) instead of spaces when
             padding is specified (see width sub-specifier).
    - Width (Optional)
      - ``(number)``: Minimum number of characters to be printed. If the value
                      to be printed is shorter than this number, the result is
                      padded with blank spaces or `0` if the `0` flag is
                      present. The value is not truncated even if the result is
                      larger. If the value is negative and the `0` flag is
                      present, the `0`s are padded after the `-` symbol.
      - `*`: The width is not specified in the format string, but as an
             additional integer value argument preceding the argument that has
             to be formatted.
    - Precision (Optional)
      - `.(number)`
        - For `d`, `i`, `o`, `u`, `x`, `X`, specifies the minimum number of
          digits to be written. If the value to be written is shorter than this
          number, the result is padded with leading zeros. The value is not
          truncated even if the result is longer.
          - A precision of `0` means that no character is written for the value
            `0`.
        - For `a`, `A`, `e`, `E`, `f`, and `F`, specifies the number of digits
          to be printed after the decimal point. By default, this is `6`.
        - For `g` and `G`, specifies the maximum number of significant digits to
          be printed.
        - For `s`, specifies the maximum number of characters to be printed. By
          default all characters are printed until the ending null character is
          encountered.
        - If the period is specified without an explicit value for precision,
          `0` is assumed.
      - `.*`: The precision is not specified in the format string, but as an
              additional integer value argument preceding the argument that has
              to be formatted.
    - Length (Optional)
      - `hh`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
              the argument will be a `signed char` or `unsigned char`. However,
              this is largely ignored in the implementation due to it not being
              necessary for Python or argument decoding (since the argument is
              always encoded at least as a 32-bit integer).
      - `h`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
             the argument will be a `signed short int` or `unsigned short int`.
             However, this is largely ignored in the implementation due to it
             not being necessary for Python or argument decoding (since the
             argument is always encoded at least as a 32-bit integer).
      - `l`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
             the argument will be a `signed long int` or `unsigned long int`.
             Also is usable with `c` and `s` to specify that the arguments will
             be encoded with `wchar_t` values (which isn't different from normal
             `char` values). However, this is largely ignored in the
             implementation due to it not being necessary for Python or argument
             decoding (since the argument is always encoded at least as a 32-bit
             integer).
      - `ll`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
              the argument will be a `signed long long int` or
              `unsigned long long int`. This is required to properly decode the
              argument as a 64-bit integer.
      - `L`: Usable with `a`, `A`, `e`, `E`, `f`, `F`, `g`, or `G` conversion
             specifiers applies to a long double argument. However, this is
             ignored in the implementation due to floating point value encoded
             that is unaffected by bit width.
      - `j`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
             the argument will be a `intmax_t` or `uintmax_t`.
      - `z`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
             the argument will be a `size_t`. This will force the argument to be
             decoded as an unsigned integer.
      - `t`: Usable with `d`, `i`, `o`, `u`, `x`, or `X` specifiers to convey
             the argument will be a `ptrdiff_t`.
      - If a length modifier is provided for an incorrect specifier, it is
        ignored.
    - Specifier (Required)
      - `d` / `i`: Used for signed decimal integers.
      - `u`: Used for unsigned decimal integers.
      - `o`: Used for unsigned decimal integers and specifies formatting should
             be as an octal number.
      - `x`: Used for unsigned decimal integers and specifies formatting should
             be as a hexadecimal number using all lowercase letters.
      - `X`: Used for unsigned decimal integers and specifies formatting should
             be as a hexadecimal number using all uppercase letters.
      - `f`: Used for floating-point values and specifies to use lowercase,
             decimal floating point formatting.
        - Default precision is `6` decimal places unless explicitly specified.
      - `F`: Used for floating-point values and specifies to use uppercase,
             decimal floating point formatting.
        - Default precision is `6` decimal places unless explicitly specified.
      - `e`: Used for floating-point values and specifies to use lowercase,
             exponential (scientific) formatting.
        - Default precision is `6` decimal places unless explicitly specified.
      - `E`: Used for floating-point values and specifies to use uppercase,
             exponential (scientific) formatting.
        - Default precision is `6` decimal places unless explicitly specified.
      - `g`: Used for floating-point values and specified to use `f` or `e`
             formatting depending on which would be the shortest representation.
        - Precision specifies the number of significant digits, not just digits
          after the decimal place.
        - If the precision is specified as `0`, it is interpreted to mean `1`.
        - `e` formatting is used if the exponent would be less than `-4` or is
          greater than or equal to the precision.
        - Trailing zeros are removed unless the `#` flag is set.
        - A decimal point only appears if it is followed by a digit.
        - `NaN` or infinities always follow `f` formatting.
      - `G`: Used for floating-point values and specified to use `f` or `e`
             formatting depending on which would be the shortest representation.
        - Precision specifies the number of significant digits, not just digits
          after the decimal place.
        - If the precision is specified as `0`, it is interpreted to mean `1`.
        - `E` formatting is used if the exponent would be less than `-4` or is
          greater than or equal to the precision.
        - Trailing zeros are removed unless the `#` flag is set.
        - A decimal point only appears if it is followed by a digit.
        - `NaN` or infinities always follow `F` formatting.
      - `c`: Used for formatting a `char` value.
      - `s`: Used for formatting a string of `char` values.
        - If width is specified, the null terminator character is included as a
          character for width count.
        - If precision is specified, no more `char`s than that value will be
          written from the string (padding is used to fill additional width).
      - `p`: Used for formatting a pointer address.
      - `%`: Prints a single `%`. Only valid as `%%` (supports no flags, width,
             precision, or length modifiers).

    Underspecified details:
    - If both `+` and ` ` flags appear, the ` ` is ignored.
    - The `+` and ` ` flags will error if used with `c` or `s`.
    - The `#` flag will error if used with `d`, `i`, `u`, `c`, `s`, or `p`.
    - The `0` flag will error if used with `c`, `s`, or `p`.
    - Both `+` and ` ` can work with the unsigned integer specifiers `u`, `o`,
      `x`, and `X`.
    - If a length modifier is provided for an incorrect specifier, it is
      ignored.
    - The `z` length modifier will decode arugments as signed as long as `d` or
      `i` is used.
    - `p` is implementation defined. For this implementation, it will print
      with a `0x` prefix and then the pointer value was printed using `%08X`.
      `p` supports the `+`, `-`, and ` ` flags, but not the `#` or `0` flags.
      None of the length modifiers are usable with `p`. This implementation will
      try to adhere to user-specified width (assuming the width provided is
      larger than the guaranteed minimum of 10). Specifying precision for `p` is
      considered an error.
    - Only `%%` is allowed with no other modifiers. Things like `%+%` will fail
      to decode. Some C stdlib implementations support any modifiers being
      present between `%`, but ignore any for the output.
    - If a width is specified with the `0` flag for a negative value, the padded
      `0`s will appear after the `-` symbol.
    - A precision of `0` for `d`, `i`, `u`, `o`, `x`, or `X` means that no
      character is written for the value `0`.
    - Precision cannot be specified for `c`.
    - Using `*` or fixed precision with the `s` specifier still requires the
      string argument to be null-terminated. This is due to argument encoding
      happening on the C/C++-side while the precision value is not read or
      otherwise used until decoding happens in this Python code.

    Non-conformant details:
    - `n` specifier: We do not support the `n` specifier since it is impossible
                     for us to retroactively tell the original program how many
                     characters have been printed since this decoding happens a
                     great deal of time after the device sent it, usually on a
                     separate processing device entirely.
    """

    # Regular expression for finding format specifiers.
    FORMAT_SPEC = re.compile(
        r'%(?P<flags>[+\- #0]+)?'
        r'(?P<width>\d+|\*)?'
        r'(?P<precision>\.(?:\d*|\*))?'
        r'(?P<length>hh|h|ll|l|j|z|t|L)?'
        r'(?P<type>[csdioxXufFeEaAgGnp%])'
    )

    # Conversions to make format strings Python compatible.
    REMAP_TYPE = {'a': 'f', 'A': 'F', 'p': 'X'}

    # Conversion specifiers by type; n is not supported.
    SIGNED_INT = frozenset('di')
    UNSIGNED_INT = frozenset('oxXup')
    FLOATING_POINT = frozenset('fFeEaAgG')

    _PACKED_FLOAT = struct.Struct('<f')

    @classmethod
    def from_string(cls, format_specifier: str):
        """Creates a FormatSpec from a str with a single format specifier."""
        match = cls.FORMAT_SPEC.fullmatch(format_specifier)

        if not match:
            raise ValueError(
                '{!r} is not a valid single format specifier'.format(
                    format_specifier
                )
            )

        return cls(match)

    def __init__(self, re_match: Match):
        """Constructs a FormatSpec from an re.Match object for FORMAT_SPEC."""
        self.match = re_match
        self.specifier: str = self.match.group()

        self.flags: str = self.match.group('flags') or ''
        self.width: str = self.match.group('width') or ''
        self.precision: str = self.match.group('precision') or ''
        self.length: str = self.match.group('length') or ''
        self.type: str = self.match.group('type')

        self.error = None
        if self.type == 'n':
            self.error = 'Unsupported conversion specifier n.'
        elif self.type == '%':
            if self.flags or self.width or self.precision or self.length:
                self.error = (
                    '%% does not support any flags, width, precision,'
                    'or length modifiers.'
                )
        elif self.type in 'csdiup' and '#' in self.flags:
            self.error = (
                '# is only supported with o, x, X, f, F, e, E, a, A, '
                'g, and G specifiers.'
            )
        elif self.type in 'csp' and '0' in self.flags:
            self.error = (
                '0 is only supported with d, i, o, u, x, X, a, A, e, '
                'E, f, F, g, and G specifiers.'
            )
        elif self.type in 'cs' and ('+' in self.flags or ' ' in self.flags):
            self.error = (
                '+ and space are only available for d, i, o, u, x, X,'
                'a, A, e, E, f, F, g, and G specifiers.'
            )
        elif self.type == 'c':
            if self.precision != '':
                self.error = 'Precision is not supported for specifier c.'
        elif self.type == 'p':
            if self.length != '':
                self.error = 'p does not support any length modifiers.'
            elif self.precision != '':
                self.error = 'p does not support precision modifiers.'

        # If we are going to add additional characters to the output, we add to
        # width_bias to ensure user-provided widths are reduced by that amount.
        self.width_bias = 0
        # Some of our machinery requires that we maintain a minimum precision
        # width to ensure a certain amount of digits gets printed. This
        # increases the user-provided precision in these cases if it was not
        # enough.
        self.minimum_precision = 0
        # Python's handling of %#o is non-standard and prepends a 0o
        # instead of single 0.
        if self.type == 'o' and '#' in self.flags:
            self.width_bias = 1
        # Python does not support %p natively.
        if self.type == 'p':
            self.width_bias = 2
            self.minimum_precision = 8

        # If we have a concrete width, we reduce it by any width bias.
        # Otherwise, we either have no width or width is *, where the decoding
        # logic will handle the width bias.
        parsed_width = int(self.width.replace('*', '') or '0')
        if parsed_width > self.width_bias:
            self.width = f'{parsed_width - self.width_bias}'

        # Python %-operator does not support `.` without a
        # trailing number. `.` is defined to be equivalent to `.0`.
        if self.precision == '.':
            self.precision = '.0'

        # If we have a concrete precision that is not *, we check that it is at
        # least minimum precision. If it is *, other parts of decoding will
        # ensure the minimum is upheld.
        if (
            self.precision != '.*'
            and int(self.precision.replace('.', '') or '0')
            < self.minimum_precision
        ):
            self.precision = f'.{self.minimum_precision}'

        # The Python %-format machinery never requires the length
        # modifier to work correctly, and it doesn't support all of the
        # C99 length format specifiers anyway. We remove it from the
        # python-compaitble format string.
        self.compatible = ''.join(
            [
                '%',
                self.flags,
                self.width,
                self.precision,
                self.REMAP_TYPE.get(self.type, self.type),
            ]
        )

    def decode(self, encoded_arg: bytes) -> DecodedArg:
        """Decodes the provided data according to this format specifier."""
        if self.error is not None:
            return DecodedArg(
                self, None, b'', DecodedArg.DECODE_ERROR, self.error
            )

        width = None
        if self.width == '*':
            width = FormatSpec.from_string('%d').decode(encoded_arg)
            encoded_arg = encoded_arg[len(width.raw_data) :]

        precision = None
        if self.precision == '.*':
            precision = FormatSpec.from_string('%d').decode(encoded_arg)
            encoded_arg = encoded_arg[len(precision.raw_data) :]

        if self.type == '%':
            return DecodedArg(
                self, (), b''
            )  # Use () as the value for % formatting.

        if self.type == 's':
            return self._merge_decoded_args(
                width, precision, self._decode_string(encoded_arg)
            )

        if self.type == 'c':
            return self._merge_decoded_args(
                width, precision, self._decode_char(encoded_arg)
            )

        if self.type in self.SIGNED_INT:
            return self._merge_decoded_args(
                width, precision, self._decode_signed_integer(encoded_arg)
            )

        if self.type in self.UNSIGNED_INT:
            return self._merge_decoded_args(
                width, precision, self._decode_unsigned_integer(encoded_arg)
            )

        if self.type in self.FLOATING_POINT:
            return self._merge_decoded_args(
                width, precision, self._decode_float(encoded_arg)
            )

        # Should be unreachable.
        assert False, f'Unhandled format specifier: {self.type}'

    def text_float_safe_compatible(self) -> str:
        return ''.join(
            [
                '%',
                self.flags.replace('0', ' '),
                self.width,
                self.precision,
                self.REMAP_TYPE.get(self.type, self.type),
            ]
        )

    def _merge_decoded_args(
        self,
        width: DecodedArg | None,
        precision: DecodedArg | None,
        main: DecodedArg,
    ) -> DecodedArg:
        def merge_optional_str(*args: str | None) -> str | None:
            return ' '.join(a for a in args if a) or None

        adjusted_width = None
        if width is not None:
            sign = -1 if width.value < 0 else 1
            adjusted_width = sign * max(0, abs(width.value) - self.width_bias)

        if width is not None and precision is not None:
            return DecodedArg(
                main.specifier,
                (
                    adjusted_width,
                    precision.value,
                    main.value,
                ),
                width.raw_data + precision.raw_data + main.raw_data,
                width.status | precision.status | main.status,
                merge_optional_str(width.error, precision.error, main.error),
            )

        if width is not None:
            return DecodedArg(
                main.specifier,
                (adjusted_width, main.value),
                width.raw_data + main.raw_data,
                width.status | main.status,
                merge_optional_str(width.error, main.error),
            )

        if precision is not None:
            return DecodedArg(
                main.specifier,
                (precision.value, main.value),
                precision.raw_data + main.raw_data,
                precision.status | main.status,
                merge_optional_str(precision.error, main.error),
            )

        return main

    def _decode_signed_integer(
        self,
        encoded: bytes,
    ) -> DecodedArg:
        """Decodes a signed variable-length integer."""
        if not encoded:
            return DecodedArg.missing(self)

        count = 0
        result = 0
        shift = 0

        for byte in encoded:
            count += 1
            result |= (byte & 0x7F) << shift

            if not byte & 0x80:
                return DecodedArg(
                    self,
                    zigzag_decode(result),
                    encoded[:count],
                    DecodedArg.OK,
                )

            shift += 7
            if shift >= 64:
                break

        return DecodedArg(
            self,
            None,
            encoded[:count],
            DecodedArg.DECODE_ERROR,
            'Unterminated variable-length integer',
        )

    def _decode_unsigned_integer(self, encoded: bytes) -> DecodedArg:
        """Decodes an unsigned variable-length integer."""
        arg = self._decode_signed_integer(encoded)
        # Since ZigZag encoding is used, unsigned integers must be masked off to
        # their original bit length.
        if arg.value is not None:
            arg.value &= (1 << self.size_bits()) - 1

        return arg

    def _decode_float(self, encoded: bytes) -> DecodedArg:
        if len(encoded) < 4:
            return DecodedArg.missing(self)

        return DecodedArg(
            self, self._PACKED_FLOAT.unpack_from(encoded)[0], encoded[:4]
        )

    def _decode_string(self, encoded: bytes) -> DecodedArg:
        """Reads a byte string from the encoded data."""
        if not encoded:
            return DecodedArg.missing(self)

        size_and_status = encoded[0]
        status = DecodedArg.OK

        if size_and_status & 0x80:
            status |= DecodedArg.TRUNCATED
            size_and_status &= 0x7F

        raw_data = encoded[0 : size_and_status + 1]
        data = raw_data[1:]

        if len(data) < size_and_status:
            status |= DecodedArg.DECODE_ERROR

        try:
            decoded = data.decode()
        except UnicodeDecodeError as err:
            return DecodedArg(
                self,
                repr(bytes(data)).lstrip('b'),
                raw_data,
                status | DecodedArg.DECODE_ERROR,
                err,
            )

        return DecodedArg(self, decoded, raw_data, status)

    def _decode_char(self, encoded: bytes) -> DecodedArg:
        """Reads an integer from the data, then converts it to a string."""
        arg = self._decode_signed_integer(encoded)

        if arg.ok():
            try:
                arg.value = chr(arg.value)
            except (OverflowError, ValueError) as err:
                arg.error = err
                arg.status |= DecodedArg.DECODE_ERROR

        return arg

    def size_bits(self) -> int:
        """Size of the argument in bits; 0 for strings."""
        if self.type == 's':
            return 0

        # TODO(hepler): 64-bit targets likely have 64-bit l, j, z, and t.
        return 64 if self.length in ['ll', 'j'] else 32

    def __str__(self) -> str:
        return self.specifier


class DecodedArg:
    """Represents a decoded argument that is ready to be formatted."""

    # Status flags for a decoded argument. These values should match the
    # DecodingStatus enum in pw_tokenizer/internal/decode.h.
    OK = 0  # decoding was successful
    MISSING = 1  # the argument was not present in the data
    TRUNCATED = 2  # the argument was truncated during encoding
    DECODE_ERROR = 4  # an error occurred while decoding the argument
    SKIPPED = 8  # argument was skipped due to a previous error

    @classmethod
    def missing(cls, specifier: FormatSpec):
        return cls(specifier, None, b'', cls.MISSING)

    def __init__(
        self,
        specifier: FormatSpec,
        value: Any,
        raw_data: bytes,
        status: int = OK,
        error=None,
    ) -> None:
        self.specifier = specifier  # FormatSpec (e.g. to represent "%0.2f")
        self.value = value  # the decoded value, or None if decoding failed
        self.raw_data = bytes(
            raw_data
        )  # the exact bytes used to decode this arg
        self._status = status
        self.error = error

    def ok(self) -> bool:
        """The argument was decoded without errors."""
        return self.status == self.OK or self.status == self.TRUNCATED

    @property
    def status(self) -> int:
        return self._status

    @status.setter
    def status(self, status: int):
        # The %% specifier is always OK and should always be printed normally.
        self._status = status if self.specifier.type != '%' else self.OK

    def _parse_format_args(self) -> tuple[int | None, int | None, Any]:
        width_val = None
        precision_val = None
        main_val = self.value

        if self.specifier.width == '*':
            if self.specifier.precision == '.*':
                width_val, precision_val, main_val = self.value
            else:
                width_val, main_val = self.value
        elif self.specifier.precision == '.*':
            precision_val, main_val = self.value

        if width_val is None and self.specifier.width:
            width_val = int(self.specifier.width)
        if precision_val is None and self.specifier.precision:
            prec_str = self.specifier.precision.lstrip('.')
            precision_val = int(prec_str) if prec_str else 0

        return width_val, precision_val, main_val

    def _format_zero_precision_zero_value(
        self, width_val: int | None, precision_val: int | None, main_val: Any
    ) -> str | None:
        """Formats zero-precision integer conversions with a value of zero."""
        is_integer_specifier_type = self.specifier.type in 'diuoxX'
        if not (
            is_integer_specifier_type and precision_val == 0 and main_val == 0
        ):
            return None

        if self.specifier.type in 'di':
            if '+' in self.specifier.flags:
                base_string = '+'
            elif ' ' in self.specifier.flags:
                base_string = ' '
            else:
                base_string = ''
        elif self.specifier.type == 'o' and '#' in self.specifier.flags:
            base_string = '0'
        else:
            base_string = ''

        if width_val is None:
            return base_string

        # We are bypassing formatting, so need the original width.
        width = abs(width_val) + self.specifier.width_bias
        pad_count = max(0, width - len(base_string))
        left_align = '-' in self.specifier.flags or width_val < 0
        if left_align:
            return base_string + ' ' * pad_count
        return ' ' * pad_count + base_string

    def format(self) -> str:
        """Returns formatted version of this argument, with error handling."""
        if self.ok():
            width_val, precision_val, main_val = self._parse_format_args()

            if self.status & self.TRUNCATED:
                main_val = main_val + '[...]'

            # Check if we are effectively .0{diuoxX} with a 0 value (this
            # includes .* with (0, 0)). C standard says a value of 0 with 0
            # precision produces an empty string.
            zero_prec_result = self._format_zero_precision_zero_value(
                width_val, precision_val, main_val
            )
            if zero_prec_result is not None:
                return zero_prec_result

            # Construct compatible specifier dynamically
            flags = self.specifier.flags
            if (
                self.specifier.type in FormatSpec.FLOATING_POINT
                and not math.isfinite(main_val)
            ):
                flags = flags.replace('0', ' ')

            compatible_spec = '%' + flags
            fmt_args = []

            if self.specifier.width == '*':
                compatible_spec += '*'
                fmt_args.append(width_val)
            elif self.specifier.width:
                compatible_spec += self.specifier.width

            # Ignore precision if unset or negative (in accordance with C99).
            if precision_val is not None and precision_val >= 0:
                if self.specifier.precision == '.*':
                    compatible_spec += '.*'
                    assert precision_val is not None
                    fmt_args.append(
                        max(precision_val, self.specifier.minimum_precision)
                    )
                elif self.specifier.precision:
                    compatible_spec += self.specifier.precision

            compatible_spec += FormatSpec.REMAP_TYPE.get(
                self.specifier.type, self.specifier.type
            )

            if len(fmt_args) == 0:
                format_value = main_val
            elif len(fmt_args) == 1:
                format_value = (fmt_args[0], main_val)
            else:
                format_value = (fmt_args[0], fmt_args[1], main_val)

            try:
                # Python has a nonstandard alternative octal form.
                if self.specifier.type == 'o' and '#' in self.specifier.flags:
                    return _format_alternative_octal(
                        compatible_spec, format_value
                    )

                # Python doesn't have a native pointer formatter.
                if self.specifier.type == 'p':
                    return _format_pointer(compatible_spec, format_value)

                return compatible_spec % format_value
            except (OverflowError, TypeError, ValueError) as err:
                self._status |= self.DECODE_ERROR
                self.error = err

        if self.status & self.SKIPPED:
            message = '{} SKIPPED'.format(self.specifier)
        elif self.status == self.MISSING:
            message = '{} MISSING'.format(self.specifier)
        elif self.status & self.DECODE_ERROR:
            message = '{} ERROR'.format(self.specifier)
        else:
            raise AssertionError(
                'Unhandled DecodedArg status {:x}!'.format(self.status)
            )

        if self.value is None or not str(self.value):
            return '<[{}]>'.format(message)

        return '<[{} ({})]>'.format(message, self.value)

    def __str__(self) -> str:
        return self.format()

    def __repr__(self) -> str:
        return f'DecodedArg({self})'


def _format_alternative_octal(compatible_spec: str, value: Any) -> str:
    """Formats an alternative octal specifier.

    This potentially throws OverflowError, TypeError, or ValueError.
    """
    compatible_specifier = compatible_spec.replace('#', '')
    result = compatible_specifier % value

    # Find index of the first non-space, non-plus, and non-zero
    # character. If we cannot find anything, we will simply
    # prepend a 0 to the formatted string.
    counter = 0
    for i, val in enumerate(result):
        if val not in ' +0':
            counter = i
            break
    return result[:counter] + '0' + result[counter:]


def _format_pointer(compatible_spec: str, value: Any) -> str:
    """Formats a pointer specifier.

    This potentially throws OverflowError, TypeError, or ValueError.
    """
    result = compatible_spec % value

    # Find the index of the first non-space character.
    first_non_space = len(result)
    for i, char in enumerate(result):
        if char != ' ':
            first_non_space = i
            break

    # If the first non-space character is a sign, insert 0x after it.
    if first_non_space < len(result) and result[first_non_space] in '+-':
        insert_idx = first_non_space + 1
    else:
        insert_idx = first_non_space

    return result[:insert_idx] + '0x' + result[insert_idx:]


def parse_format_specifiers(format_string: str) -> Iterable[FormatSpec]:
    for spec in FormatSpec.FORMAT_SPEC.finditer(format_string):
        yield FormatSpec(spec)


class FormattedString(NamedTuple):
    value: str
    args: Sequence[DecodedArg]
    remaining: bytes

    def ok(self) -> bool:
        """Arg data decoded successfully and all expected args were found."""
        return all(arg.ok() for arg in self.args) and not self.remaining

    def score(self, date_removed: datetime | None = None) -> tuple:
        """Returns a key for sorting by how successful a decode was.

        Decoded strings are sorted by whether they

          1. decoded all bytes for all arguments without errors,
          2. decoded all data,
          3. have the fewest decoding errors,
          4. decoded the most arguments successfully, or
          5. have the most recent removal date, if they were removed.

        This must match the collision resolution logic in detokenize.cc.

        To format a list of FormattedStrings from most to least successful,
        use sort(key=FormattedString.score, reverse=True).
        """
        # Count arguments excluding %% since they are not decoded.
        args_decoded = sum(1 for a in self.args if a.specifier.type != '%')

        return (
            self.ok(),  # decocoded all data and all expected args were found
            not self.remaining,  # decoded all data
            -sum(not arg.ok() for arg in self.args),  # fewest errors
            args_decoded,  # decoded the most arguments
            date_removed or datetime.max,
        )  # most recently present


class FormatString:
    """Represents a printf-style format string."""

    def __init__(self, format_string: str):
        """Parses format specifiers in the format string."""
        self.format_string = format_string
        self.specifiers = tuple(parse_format_specifiers(self.format_string))

        # List of non-specifier string pieces with room for formatted arguments.
        self._segments = self._parse_string_segments()

    def _parse_string_segments(self) -> list:
        """Splits the format string by format specifiers."""
        if not self.specifiers:
            return [self.format_string]

        spec_spans = [spec.match.span() for spec in self.specifiers]

        # Start with the part of the format string up to the first specifier.
        string_pieces = [self.format_string[: spec_spans[0][0]]]

        for (_, end1), (start2, _) in zip(spec_spans[:-1], spec_spans[1:]):
            string_pieces.append(self.format_string[end1:start2])

        # Append the format string segment after the last format specifier.
        string_pieces.append(self.format_string[spec_spans[-1][1] :])

        # Make a list with spots for the replacements between the string pieces.
        segments: list = [None] * (len(string_pieces) + len(self.specifiers))
        segments[::2] = string_pieces

        return segments

    def decode(self, encoded: bytes) -> tuple[Sequence[DecodedArg], bytes]:
        """Decodes arguments according to the format string.

        Args:
          encoded: bytes; the encoded arguments

        Returns:
          tuple with the decoded arguments and any unparsed data
        """
        decoded_args = []

        last_good_index: int | None = None
        fatal_error = False
        index = 0

        for spec in self.specifiers:
            arg = spec.decode(encoded[index:])

            if fatal_error:
                # After an error is encountered, continue to attempt to parse
                # arguments, but mark them all as SKIPPED. If an error occurs,
                # it's impossible to know if subsequent arguments are valid.
                arg.status |= DecodedArg.SKIPPED
            elif not arg.ok():
                fatal_error = True
                last_good_index = index

            decoded_args.append(arg)
            index += len(arg.raw_data)

        return tuple(decoded_args), encoded[last_good_index or index :]

    def format(
        self, encoded_args: bytes, show_errors: bool = False
    ) -> FormattedString:
        """Decodes arguments and formats the string with them.

        Args:
          encoded_args: the arguments to decode and format the string with
          show_errors: if True, an error message is used in place of the %
              conversion specifier when an argument fails to decode

        Returns:
          tuple with the formatted string, decoded arguments, and remaining data
        """
        # Insert formatted arguments in place of each format specifier.
        args, remaining = self.decode(encoded_args)

        if show_errors:
            self._segments[1::2] = (arg.format() for arg in args)
        else:
            self._segments[1::2] = (
                arg.format() if arg.ok() else arg.specifier.specifier
                for arg in args
            )

        return FormattedString(''.join(self._segments), args, remaining)


def decode(
    format_string: str, encoded_arguments: bytes, show_errors: bool = False
) -> str:
    """Decodes arguments and formats them with the provided format string.

    Args:
      format_string: the printf-style format string
      encoded_arguments: encoded arguments with which to format
          format_string; must exclude the 4-byte string token
      show_errors: if True, an error message is used in place of the %
          conversion specifier when an argument fails to decode

    Returns:
      the printf-style formatted string
    """
    return (
        FormatString(format_string).format(encoded_arguments, show_errors).value
    )
