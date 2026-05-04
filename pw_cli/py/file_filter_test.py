#!/usr/bin/env python3
# Copyright 2024 The Pigweed Authors
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
"""Tests for file filtering."""

import dataclasses
import re
import unittest

from pw_cli.file_filter import FileFilter


class TestFileFilter(unittest.TestCase):
    """Test FileFilter class"""

    @dataclasses.dataclass
    class TestData:
        filter: FileFilter
        value: str
        expected: bool

    TEST_SCENARIOS = (
        TestData(FileFilter(endswith=('bar', 'foo')), 'foo', True),
        TestData(FileFilter(endswith=('bar', 'boo')), 'foo', False),
        TestData(
            FileFilter(exclude=(re.compile('a/.+'),), name=('foo',)),
            '/a/b/c/foo',
            False,
        ),
        TestData(
            FileFilter(exclude=(re.compile('x/.+'),), name=('foo',)),
            '/a/b/c/foo',
            True,
        ),
        TestData(
            FileFilter(exclude=(re.compile('a+'), re.compile('b+'))),
            'cccc',
            True,
        ),
        TestData(FileFilter(name=('foo',)), 'foo', True),
        TestData(FileFilter(name=('foo',)), 'food', False),
        TestData(FileFilter(name=(re.compile('foo'),)), 'foo', True),
        TestData(FileFilter(name=(re.compile('foo'),)), 'food', False),
        TestData(FileFilter(name=(re.compile('fo+'),)), 'foo', True),
        TestData(FileFilter(name=(re.compile('fo+'),)), 'fd', False),
        TestData(FileFilter(suffix=('.exe',)), 'a/b.py/foo.exe', True),
        TestData(FileFilter(suffix=('.py',)), 'a/b.py/foo.exe', False),
        TestData(FileFilter(suffix=('.exe',)), 'a/b.py/foo.py.exe', True),
        TestData(FileFilter(suffix=('.py',)), 'a/b.py/foo.py.exe', False),
        TestData(FileFilter(suffix=('.a', '.b')), 'foo.b', True),
        TestData(FileFilter(suffix=('.a', '.b')), 'foo.c', False),
    )

    def test_matches(self):
        for test_num, test_data in enumerate(self.TEST_SCENARIOS):
            with self.subTest(i=test_num):
                self.assertEqual(
                    test_data.filter.matches(test_data.value),
                    test_data.expected,
                )

    def test_concat_with_another_filter(self):
        filter1 = FileFilter(name=['foo'], suffix=['.txt'])
        filter2 = FileFilter(name=['bar'], suffix=['.log'])
        combined = filter1.concat(filter2)

        self.assertEqual([p.pattern for p in combined.exclude], [])
        self.assertEqual(combined.endswith, frozenset())
        self.assertEqual(set(p.pattern for p in combined.name), {'foo', 'bar'})
        self.assertEqual(set(combined.suffix), {'.txt', '.log'})

    def test_concat_with_kwargs(self):
        filter1 = FileFilter(name=['foo'], suffix=['.txt'])
        combined = filter1.concat(name=['bar'], suffix=['.log'])

        self.assertEqual([p.pattern for p in combined.exclude], [])
        self.assertEqual(combined.endswith, frozenset())
        self.assertEqual(set(p.pattern for p in combined.name), {'bar', 'foo'})
        self.assertEqual(set(combined.suffix), {'.log', '.txt'})

    def test_concat_with_filter_and_kwargs(self):
        filter1 = FileFilter(name=['foo'], suffix=['.txt'])
        filter2 = FileFilter(name=['bar'], suffix=['.log'])
        combined = filter1.concat(filter2, name=['baz'], suffix=['.md'])

        self.assertEqual([p.pattern for p in combined.exclude], [])
        self.assertEqual(combined.endswith, frozenset())
        self.assertEqual(
            set(p.pattern for p in combined.name), {'baz', 'foo', 'bar'}
        )
        self.assertEqual(set(combined.suffix), {'.md', '.txt', '.log'})

    def test_concat_excludes(self):
        filter1 = FileFilter(exclude=[re.compile('a/.*')])
        filter2 = FileFilter(exclude=[re.compile('b/.*')])
        combined = filter1.concat(filter2, exclude=['c/.*'])

        self.assertEqual(
            set(p.pattern for p in combined.exclude), {'c/.*', 'a/.*', 'b/.*'}
        )
        self.assertEqual(combined.endswith, frozenset())
        self.assertEqual([p.pattern for p in combined.name], [])
        self.assertEqual(combined.suffix, frozenset())

    def test_concat_empty(self):
        filter1 = FileFilter(name=['foo'])
        combined = filter1.concat()

        self.assertIsNot(filter1, combined, 'Must be a new object')
        self.assertEqual([p.pattern for p in combined.exclude], [])
        self.assertEqual(combined.endswith, frozenset())
        self.assertEqual([p.pattern for p in combined.name], ['foo'])
        self.assertEqual(combined.suffix, frozenset())

    def test_equality_with_flags(self):
        filter1 = FileFilter(exclude=[re.compile('a', re.IGNORECASE)])
        filter2 = FileFilter(exclude=[re.compile('a')])
        self.assertNotEqual(filter1, filter2)
        self.assertNotEqual(hash(filter1), hash(filter2))

    def test_equality_with_matching_flags(self):
        filter1 = FileFilter(exclude=[re.compile('a', re.IGNORECASE)])
        filter2 = FileFilter(exclude=[re.compile('a', re.IGNORECASE)])
        self.assertIsNot(filter1.exclude, filter2.exclude)
        self.assertEqual(filter1, filter2)
        self.assertEqual(hash(filter1), hash(filter2))

    def test_equality_string_and_pattern(self):
        filter1 = FileFilter(exclude=['a'])
        filter2 = FileFilter(exclude=[re.compile('a')])
        self.assertIsNot(filter1.exclude, filter2.exclude)
        self.assertEqual(filter1, filter2)
        self.assertEqual(hash(filter1), hash(filter2))

    def test_concat_with_string_args(self):
        filter1 = FileFilter(name=['foo'])
        combined = filter1.concat(endswith='.h', exclude='out')

        self.assertEqual(combined.endswith, frozenset(['.h']))
        self.assertEqual(set(p.pattern for p in combined.exclude), {'out'})
        self.assertEqual([p.pattern for p in combined.name], ['foo'])

    def test_init_with_string_args(self):
        filter1 = FileFilter(
            endswith='.h',
            exclude='out',
            name='foo',
            suffix='.txt',
        )
        self.assertEqual(filter1.endswith, frozenset(['.h']))
        self.assertEqual(set(p.pattern for p in filter1.exclude), {'out'})
        self.assertEqual(set(p.pattern for p in filter1.name), {'foo'})
        self.assertEqual(filter1.suffix, frozenset(['.txt']))

    def test_init_with_string_args_does_not_split(self):
        filter1 = FileFilter(endswith='foo')
        # If it split, it would match 'baf' because it ends with 'f'.
        self.assertFalse(filter1.matches('baf'))
        self.assertTrue(filter1.matches('foo'))
        self.assertTrue(filter1.matches('barfoo'))


if __name__ == '__main__':
    unittest.main()
