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
    TestCaseBytes(b'\xAA\xAA\xAA\xAA\xfc\x01', '~!'),
    TestCaseBytes(b'\xCC\xCC\xCC\xCC\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xDD\xDD\xDD\xDD\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xDD\xDD\xDD\xDD\xfe\xff\xff\xff\x1f', '4294967295!'),
    TestCaseBytes(b'\xEE\xEE\xEE\xEE\xfe\xff\x07', '65535!'),
    TestCaseBytes(b'\xEE\xEE\xEE\xEE\xfe\xff\xff\xff\x1f', '4294967295!'),
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
    b'\xFF\x00\x00\x00----'
    b'\x00\x00\x00\x01----'
    b'\xd3\x4d\x34\xd3----'
    b'\xFF\xEE\xEE\xDD----'
    b'\xEE\xEE\xEE\xEE----'
    b'\x9D\xA7\x97\xF8----'
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
    b'\x09\x00\x00\x00'
    b'\0\0\x00\x00'
    b'\x00\x00\x00\x00----'
    b'\x0A\x0B\x0C\x0D----'
    b'\x0E\x0F\x00\x01----'
    b'\xAA\xAA\xAA\xAA----'
    b'\xBB\xBB\xBB\xBB----'
    b'\xCC\xCC\xCC\xCC----'
    b'\xDD\xDD\xDD\xDD----'
    b'\xEE\xEE\xEE\xEE----'
    b'\xFF\xFF\xFF\xFF----'
    b'\0'
    b'Use the %s, %s.\0'
    b'Now there are %d of %s!\0'
    b'%c!\0'
    b'%hhu!\0'
    b'%hu!\0'
    b'%u!\0'
    b'%lu!\0'
    b'%llu!'
)

DATA_WITH_COLLISIONS = (
    b'TOKENS\0\0'
    b'\x0F\x00\x00\x00'
    b'\0\0\x00\x00'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\x01\x02\x03\x04'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\x00\x00\x00\x00\xff\xff\xff\xff'
    b'\xAA\xAA\xAA\xAA\x00\x00\x00\x00'
    b'\xAA\xAA\xAA\xAA\xff\xff\xff\xff'
    b'\xBB\xBB\xBB\xBB\xff\xff\xff\xff'
    b'\xBB\xBB\xBB\xBB\xff\xff\xff\xff'
    b'\xCC\xCC\xCC\xCC\xff\xff\xff\xff'
    b'\xCC\xCC\xCC\xCC\xff\xff\xff\xff'
    b'\xDD\xDD\xDD\xDD\xff\xff\xff\xff'
    b'\xDD\xDD\xDD\xDD\xff\xff\xff\xff'
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
