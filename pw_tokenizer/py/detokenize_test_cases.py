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
"""Defines shared test cases and databases for detokenizer data-driven tests."""

from typing import NamedTuple


class TestCase(NamedTuple):
    data: str
    expected: str


class TestCaseBytes(NamedTuple):
    data: bytes
    expected: str


ONE = '$AQAAAA=='
TWO = '$BQAAAA=='
THREE = '$/wAAAA=='
FOUR = '$/+7u3Q=='
NEST_ONE = '$7u7u7g=='

TEST_CASES = (
    'Base64 no arguments',
    TestCase(ONE, 'One'),
    TestCase(TWO, 'TWO'),
    TestCase(THREE, '333'),
    TestCase(FOUR, 'FOUR'),
    TestCase(f'{FOUR}{ONE}{ONE}', 'FOUROneOne'),
    TestCase(f'{ONE}{TWO}{THREE}{FOUR}', 'OneTWO333FOUR'),
    TestCase(
        f'{ONE}\r\n{TWO}\r\n{THREE}\r\n{FOUR}\r\n',
        'One\r\nTWO\r\n333\r\nFOUR\r\n',
    ),
    TestCase(f'123{FOUR}', '123FOUR'),
    TestCase(f'123{FOUR}, 56', '123FOUR, 56'),
    TestCase(f'12{THREE}{FOUR}, 56', '12333FOUR, 56'),
    TestCase(f'$0{ONE}', '$0One'),
    TestCase('$/+7u3Q=', '$/+7u3Q='),
    TestCase(f'$123456=={FOUR}', '$123456==FOUR'),
    TestCase(NEST_ONE, 'One'),
    TestCase(f'{NEST_ONE}{NEST_ONE}{NEST_ONE}', 'OneOneOne'),
    TestCase(f'{FOUR}${ONE}{NEST_ONE}?', 'FOUR$OneOne?'),
    TestCase('$16==', 'd7 encodes as 16=='),
    TestCase('${unknown domain}16==', '${unknown domain}16=='),
    TestCase('${}16==', 'd7 encodes as 16=='),
    TestCase('${ }16==', 'd7 encodes as 16=='),
    TestCase('${\r\t\n }16==', 'd7 encodes as 16=='),
    TestCase('$64==', '$64==++++'),
    TestCase('$AgAAAA==', 'My arg %s %d'),
    TestCase('$#0000000a', 'Active message'),
    TestCase('$10#0000000010', 'Active message'),
    TestCase('$CgAAAA==', 'Active message'),
    TestCase('$#0000000b', '$#0000000b'),
    TestCase('$10#0000000011', '$10#0000000011'),
    TestCase('$CwAAAA==', '$CwAAAA=='),
    TestCase('$AAAAAaAA', 'Base64 is valid hexadecimal 16'),
    TestCase('$64#AAAAAaAA', 'Base64 is valid hexadecimal 16'),
    TestCase('$000000000000', 'Base64 is valid base 10 -39 26 -4970 26'),
    TestCase('$64#000000000000', 'Base64 is valid base 10 -39 26 -4970 26'),
)

OPTIONALLY_TOKENIZED_TEST_CASES = (
    'Optionally tokenized data',
    TestCaseBytes(b'\x01\x00\x00\x00', 'One'),
)

WITH_ARGS_SUCCESSFUL_CASES_BINARY = (
    'With args successful binary',
    TestCaseBytes(b'\x0a\x0b\x0c\x0d\x05force\x04Luke', 'Use the force, Luke.'),
    TestCaseBytes(b'\x0e\x0f\x00\x01\x04\x04them', 'Now there are 2 of them!'),
    TestCaseBytes(
        b'\x0e\x0f\x00\x01\x80\x01\x04them', 'Now there are 64 of them!'
    ),
    TestCaseBytes(b'\x01\x00\x0d\xf0\xd0\x0f\x49\x40', '3.141590'),
    TestCaseBytes(b'\x01\x00\x0d\xf0\xd0\x0f\x49\xc0', '-3.141590'),
    TestCaseBytes(b'\x01\x00\x0d\xf0\x00\x00\x00\x00', '0.000000'),
    TestCaseBytes(b'\x02\x00\x0d\xf0\xd0\x0f\x49\x40', '3.14'),
    TestCaseBytes(b'\x03\x00\x0d\xf0\xd0\x0f\x49\x40', '  3.1'),
    TestCaseBytes(b'\x04\x00\x0d\xf0\xd0\x0f\x49\x40', '+3.141590'),
    TestCaseBytes(b'\x05\x00\x0d\xf0\xd0\x0f\x49\x40', '3'),
    TestCaseBytes(b'\x50\x00\x0d\xf0\x82\x01', 'Char A'),
    TestCaseBytes(b'\x60\x00\x0d\xf0\x00', 'String '),
    TestCaseBytes(b'\x60\x00\x0d\xf0\x05\x68\x65\x6c\x6c\x6f', 'String hello'),
    TestCaseBytes(b'\x60\x00\x0d\xf0\x83\x61\x62\x63', 'String abc[...]'),
    TestCaseBytes(b'\x70\x00\x0d\xf0\x09', '-5!'),
    TestCaseBytes(b'\x70\x00\x0d\xf0\xfe\x01', '127!'),
    TestCaseBytes(b'\x71\x00\x0d\xf0\xff\xff\x03', '-32768!'),
    TestCaseBytes(b'\x72\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x73\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x74\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x75\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x76\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x80\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x81\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(b'\x82\x00\x0d\xf0\xc0\x9a\x0c', '100000!'),
    TestCaseBytes(
        b'\x83\x00\x0d\xf0\x01\xe7\x07\xfe\x03\xfe\xff\x07',
        '-1 -500 255 65535',
    ),
    TestCaseBytes(
        b'\x2e\xd2\x8b\x6a\x04\xff\xbf\xff\xff\x0f', 'TI2: CCI=0x80001000'
    ),
    TestCaseBytes(b'\xbb\xbb\xbb\xbb\xfe\x03', '255!'),
    TestCaseBytes(b'\xaa\xaa\xaa\xaa\xfc\x01', '~!'),
    TestCaseBytes(b'\xcc\xcc\xcc\xcc\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xdd\xdd\xdd\xdd\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xdd\xdd\xdd\xdd\xfe\xff\xff\xff\x1f', '4294967295!'),
    TestCaseBytes(b'\xee\xee\xee\xee\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xee\xee\xee\xee\xfe\xff\xff\xff\x1f', '4294967295!'),
    TestCaseBytes(b'\xff\xff\xff\xff\xfe\xff\xff\xff\x1f', '4294967295!'),
)

WITH_ARGS_STAR_CASES_BINARY = (
    'With args star binary',
    TestCaseBytes(b'\xe4\x1e\x7d\x41\x0a\x54', '   42'),
    TestCaseBytes(b'\xe4\x1e\x7d\x41\x09\x54', '42   '),
    TestCaseBytes(b'\x16\xbf\x18\x3c\x06\x05hello', 'hel'),
    TestCaseBytes(b'\xe0\x6b\x83\x6b\x10\x04\xd0\x0f\x49\x40', '    3.14'),
    TestCaseBytes(b'\x03\xf0\x46\x3f\x0a\x54', '%**d'),
    TestCaseBytes(b'\x5e\x19\xe6\xad\x10\x04\xd0\x0f\x49\x40', '%*.*.*f'),
    TestCaseBytes(b'\xe0\x6b\x83\x6b\x10\x03\xd0\x0f\x49\x40', '3.141590'),
)

WITH_ARGS_PERCENT_G_CASES_BINARY = (
    'With args percent g binary',
    TestCaseBytes(b'\x30\x00\x0d\xf0\x00\x00\x20\x40', 'Short 2.5'),
    TestCaseBytes(b'\x31\x00\x0d\xf0\x00\x00\x20\x40', 'Short 2.5'),
)

WITH_ARGS_PERCENT_E_CASES_BINARY = (
    'With args percent e binary',
    TestCaseBytes(b'\x20\x00\x0d\xf0\x00\x00\x20\x40', 'Exp 2.500000e+00'),
    TestCaseBytes(b'\x21\x00\x0d\xf0\x00\x00\x20\x40', 'Exp 2.500000E+00'),
)

WITH_COLLISIONS_CASES_BINARY = (
    'With collisions binary',
    TestCaseBytes(b'\x00\x00\x00\x00', 'This string is present'),
    TestCaseBytes(b'\x00\x00\x00\x00\x01', 'One arg -1'),
    TestCaseBytes(b'\x00\x00\x00\x00\x80', 'One arg [...]'),
    TestCaseBytes(b'\x00\x00\x00\x00\x04Hey!\x04', 'Two args Hey! 2'),
    TestCaseBytes(b'\x00\x00\x00\x00\x80\x80\x80\x80\x00', 'Two args [...] 0'),
    TestCaseBytes(b'\x00\x00\x00\x00\x08?', 'One arg %s'),
    TestCaseBytes(b'\x00\x00\x00\x00\x01!\x02\xCE\xB1', 'Two args ! α % % %'),
    TestCaseBytes(b'\xbb\xbb\xbb\xbb\x00', 'Two ints 0 %d'),
    TestCaseBytes(b'\xcc\xcc\xcc\xcc\x02Yo\x05?', 'Two strings Yo %s'),
    TestCaseBytes(b'\x00\x00\x00\x00\x01\x00\x01\x02', 'Four args -1 0 -1 1'),
    TestCaseBytes(b'\xaa\xaa\xaa\xaa', 'This one is present'),
)

# Databases
TEST_DATABASE = (
    b'TOKENS\0\0'
    b'\x0f\x00\x00\x00'  # Number of tokens in this database (15).
    b'\0\0\x00\x00'
    b'\x01\x00\x00\x00----'
    b'\x02\x00\x00\x00\xff\xff\xff\xff'
    b'\x05\x00\x00\x00----'
    b'\x0a\x00\x00\x00\x01\x01\xe4\x07'
    b'\x0a\x00\x00\x00\xff\xff\xff\xff'
    b'\x0b\x00\x00\x00\xff\xff\xff\xff'
    b'\x0b\x00\x00\x00\xff\xff\xff\xff'
    b'\xd7\x00\x00\x00----'
    b'\xeb\x00\x00\x00----'
    b'\xff\x00\x00\x00----'
    b'\x00\x00\x00\x01----'
    b'\xd3\x4d\x34\xd3----'
    b'\xff\xee\xee\xdd----'
    b'\xee\xee\xee\xee----'
    b'\x9d\xa7\x97\xf8----'
    b'One\0'
    b'My arg %s %d\0'
    b'TWO\0'
    b'Old message\0'
    b'Active message\0'
    b'Active message 1\0'
    b'Active message 2\0'
    b'd7 encodes as 16==\0'
    b'$64==+\0'  # recursively decodes to itself with a + after it
    b'333\0'
    b'Base64 is valid hexadecimal %d\0'
    b'Base64 is valid base 10 %d %d %d %d\0'
    b'FOUR\0'
    b'$AQAAAA==\0'
    b'\xe2\x96\xa0msg\xe2\x99\xa6This is $AQAAAA== message\xe2\x96\xa0'
    b'module\xe2\x99\xa6\xe2\x96\xa0file\xe2\x99\xa6file.txt'
)

DATA_WITH_ARGUMENTS = (
    b'TOKENS\0\0'
    b'\x28\0\0\0'
    b'\0\0\x00\x00'
    b'\x00\x00\x00\x00----'  # 01
    b'\x0e\x0f\x00\x01----'  # 02
    b'\x0a\x0b\x0c\x0d----'  # 03
    b'\x16\xbf\x18\x3c----'  # 04
    b'\x03\xf0\x46\x3f----'  # 05
    b'\xe4\x1e\x7d\x41----'  # 06
    b'\xe0\x6b\x83\x6b----'  # 07
    b'\xaa\xaa\xaa\xaa----'  # 08
    b'\x5e\x19\xe6\xad----'  # 09
    b'\xbb\xbb\xbb\xbb----'  # 10
    b'\xcc\xcc\xcc\xcc----'  # 11
    b'\xdd\xdd\xdd\xdd----'  # 12
    b'\xee\xee\xee\xee----'  # 13
    b'\x01\x00\x0d\xf0----'  # 14
    b'\x02\x00\x0d\xf0----'  # 15
    b'\x03\x00\x0d\xf0----'  # 16
    b'\x04\x00\x0d\xf0----'  # 17
    b'\x05\x00\x0d\xf0----'  # 18
    b'\x10\x00\x0d\xf0----'  # 19
    b'\x20\x00\x0d\xf0----'  # 20
    b'\x21\x00\x0d\xf0----'  # 21
    b'\x30\x00\x0d\xf0----'  # 22
    b'\x31\x00\x0d\xf0----'  # 23
    b'\x40\x00\x0d\xf0----'  # 24
    b'\x41\x00\x0d\xf0----'  # 25
    b'\x50\x00\x0d\xf0----'  # 26
    b'\x60\x00\x0d\xf0----'  # 27
    b'\x70\x00\x0d\xf0----'  # 28
    b'\x71\x00\x0d\xf0----'  # 29
    b'\x72\x00\x0d\xf0----'  # 30
    b'\x73\x00\x0d\xf0----'  # 31
    b'\x74\x00\x0d\xf0----'  # 32
    b'\x75\x00\x0d\xf0----'  # 33
    b'\x76\x00\x0d\xf0----'  # 34
    b'\x80\x00\x0d\xf0----'  # 35
    b'\x81\x00\x0d\xf0----'  # 36
    b'\x82\x00\x0d\xf0----'  # 37
    b'\x83\x00\x0d\xf0----'  # 38
    b'\x2e\xd2\x8b\x6a----'  # 39
    b'\xff\xff\xff\xff----'  # 40
    b'\0'  # 01
    b'Now there are %d of %s!\0'  # 02
    b'Use the %s, %s.\0'  # 03
    b'%.*s\0'  # 04
    b'%**d\0'  # 05
    b'%*d\0'  # 06
    b'%*.*f\0'  # 07
    b'%c!\0'  # 08
    b'%*.*.*f\0'  # 09
    b'%hhu!\0'  # 10
    b'%hu!\0'  # 11
    b'%u!\0'  # 12
    b'%lu!\0'  # 13
    b'%f\0'  # 14
    b'%.2f\0'  # 15
    b'%5.1f\0'  # 16
    b'%+f\0'  # 17
    b'%.0f\0'  # 18
    b'Pointer %p\0'  # 19
    b'Exp %e\0'  # 20
    b'Exp %E\0'  # 21
    b'Short %g\0'  # 22
    b'Short %G\0'  # 23
    b'HexFloat %a\0'  # 24
    b'HexFloat %A\0'  # 25
    b'Char %c\0'  # 26
    b'String %s\0'  # 27
    b'%hhd!\0'  # 28
    b'%hd!\0'  # 29
    b'%ld!\0'  # 30
    b'%lld!\0'  # 31
    b'%jd!\0'  # 32
    b'%zd!\0'  # 33
    b'%td!\0'  # 34
    b'%ju!\0'  # 35
    b'%zu!\0'  # 36
    b'%tu!\0'  # 37
    b'%hhd %hd %hhu %hu\0'  # 38
    b'TI%d: CCI=0x%x\0'  # 39
    b'%llu!\0'  # 40
)

DATA_WITH_COLLISIONS = (
    b'TOKENS\0\0'
    b'\x0f\x00\x00\x00'
    b'\0\0\x00\x00'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\x01\x02\x03\x04'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\xaa\xaa\xaa\xaa\x00\x00\x00\x00'
    b'\xaa\xaa\xaa\xaa\xff\xff\xff\xff'
    b'\xbb\xbb\xbb\xbb\xff\xff\xff\xff'
    b'\xbb\xbb\xbb\xbb\xff\xff\xff\xff'
    b'\xcc\xcc\xcc\xcc\xff\xff\xff\xff'
    b'\xcc\xcc\xcc\xcc\xff\xff\xff\xff'
    b'\xdd\xdd\xdd\xdd\xff\xff\xff\xff'
    b'\xdd\xdd\xdd\xdd\xff\xff\xff\xff'
    b'This string is present\0'
    b'This string is removed\0'
    b'One arg %d\0'
    b'One arg %s\0'
    b'Two args %s %u\0'
    b'Two args %s %s %% %% %%\0'
    b'Four args %d %d %d %d\0'
    b'This one is removed\0'
    b'This one is present\0'
    b'Two ints %d %d\0'
    b'Three ints %d %d %d\0'
    b'Three strings %s %s %s\0'
    b'Two strings %s %s\0'
    b'Three %s %s %s\0'
    b'Five %d %d %d %d %s\0'
)
