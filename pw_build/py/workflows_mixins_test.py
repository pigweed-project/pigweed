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
"""Tests for merging mix-in build configs."""

import re
import unittest

from google.protobuf import any_pb2

from pw_build.proto import pigweed_build_driver_pb2, workflows_pb2
from pw_build.workflows.private.mixins import merge_build_configs

_BAZEL_OPTIONS_TYPE = 'pw.build.proto.BazelDriverOptions'


def _bazel_options(**kwargs) -> any_pb2.Any:
    """Packs BazelDriverOptions the same way workflows.json files do."""
    options = pigweed_build_driver_pb2.BazelDriverOptions(**kwargs)
    return any_pb2.Any(
        type_url=_BAZEL_OPTIONS_TYPE, value=options.SerializeToString()
    )


_BASE = workflows_pb2.BuildConfig(
    name='base',
    description='Base config',
    build_type='fake_build_type',
    args=['--base'],
    env={'SHARED': 'base', 'BASE_ONLY': '1'},
    driver_options=_bazel_options(project_working_subdir='sub/dir'),
)
_PLAIN = workflows_pb2.BuildConfig(name='plain', build_type='fake_build_type')
_M1 = workflows_pb2.BuildConfig(
    name='m1',
    mixin=True,
    build_type='fake_build_type',
    args=['--m1'],
    env={'SHARED': 'm1', 'M1_ONLY': '1'},
    driver_options=_bazel_options(no_test=True),
)
_M2 = workflows_pb2.BuildConfig(
    name='m2', mixin=True, args=['--m2'], env={'SHARED': 'm2'}
)
# Options whose type is set but whose fields all have default values.
_EMPTY_BAZEL_OPTIONS = any_pb2.Any(type_url=_BAZEL_OPTIONS_TYPE)
_UNREGISTERED_OPTIONS = any_pb2.Any(
    type_url='pw.test.Unregistered', value=b'\x08\x01'
)


class MergeBuildConfigsTest(unittest.TestCase):
    """Tests for merge_build_configs()."""

    def test_merge(self):
        """Mix-ins are layered onto the base in order."""
        merged_options = _bazel_options(
            no_test=True, project_working_subdir='sub/dir'
        )
        cases = [
            (
                [_M1, _M2],
                workflows_pb2.BuildConfig(
                    name='base+m1+m2',
                    description='Base config',
                    build_type='fake_build_type',
                    args=['--base', '--m1', '--m2'],
                    env={'SHARED': 'm2', 'BASE_ONLY': '1', 'M1_ONLY': '1'},
                    driver_options=merged_options,
                ),
            ),
            (
                [_M2, _M1],
                workflows_pb2.BuildConfig(
                    name='base+m2+m1',
                    description='Base config',
                    build_type='fake_build_type',
                    args=['--base', '--m2', '--m1'],
                    env={'SHARED': 'm1', 'BASE_ONLY': '1', 'M1_ONLY': '1'},
                    driver_options=merged_options,
                ),
            ),
        ]
        for mixins, expected in cases:
            with self.subTest([mixin.name for mixin in mixins]):
                self.assertEqual(merge_build_configs(_BASE, mixins), expected)

    def test_merge_driver_options(self):
        """Driver options set on only one side, or with no fields, are kept."""
        base_with_empty_options = workflows_pb2.BuildConfig(
            name='base',
            build_type='fake_build_type',
            driver_options=_EMPTY_BAZEL_OPTIONS,
        )
        mixin_with_empty_options = workflows_pb2.BuildConfig(
            name='mixin', mixin=True, driver_options=_EMPTY_BAZEL_OPTIONS
        )
        cases = [
            ('only on base', _BASE, _M2, _BASE.driver_options),
            ('only on mixin', _PLAIN, _M1, _M1.driver_options),
            (
                'no fields on base',
                base_with_empty_options,
                _M1,
                _M1.driver_options,
            ),
            (
                'no fields on mixin',
                _BASE,
                mixin_with_empty_options,
                _BASE.driver_options,
            ),
        ]
        for name, base, mixin, expected in cases:
            with self.subTest(name):
                merged = merge_build_configs(base, [mixin])
                self.assertEqual(merged.driver_options, expected)

    def test_merge_does_not_modify_inputs(self):
        """The base and mix-ins passed in are left unchanged."""
        base, m1 = workflows_pb2.BuildConfig(), workflows_pb2.BuildConfig()
        base.CopyFrom(_BASE)
        m1.CopyFrom(_M1)
        merge_build_configs(base, [m1])
        self.assertEqual((base, m1), (_BASE, _M1))

    def test_merge_clears_mixin_flag(self):
        """The merged config is never marked as a mix-in."""
        self.assertFalse(merge_build_configs(_M1, [_M2]).mixin)

    def test_merge_errors(self):
        """Incompatible mix-ins raise ValueError."""
        cases = [
            (
                'build_type mismatch',
                _BASE,
                workflows_pb2.BuildConfig(
                    name='other', mixin=True, build_type='other'
                ),
                'build_type `other` does not match base build_type '
                '`fake_build_type`',
            ),
            (
                'driver_options type mismatch',
                _BASE,
                workflows_pb2.BuildConfig(
                    name='other',
                    mixin=True,
                    driver_options=_UNREGISTERED_OPTIONS,
                ),
                'driver_options type `pw.test.Unregistered` does not match '
                f'`{_BAZEL_OPTIONS_TYPE}`',
            ),
            (
                'driver_options type mismatch with no fields',
                _BASE,
                workflows_pb2.BuildConfig(
                    name='other',
                    mixin=True,
                    driver_options=any_pb2.Any(type_url='pw.test.Other'),
                ),
                'driver_options type `pw.test.Other` does not match '
                f'`{_BAZEL_OPTIONS_TYPE}`',
            ),
            (
                'driver_options type mismatch with no fields on base',
                workflows_pb2.BuildConfig(
                    name='base', driver_options=_EMPTY_BAZEL_OPTIONS
                ),
                workflows_pb2.BuildConfig(
                    name='other',
                    mixin=True,
                    driver_options=_UNREGISTERED_OPTIONS,
                ),
                'driver_options type `pw.test.Unregistered` does not match '
                f'`{_BAZEL_OPTIONS_TYPE}`',
            ),
            (
                'unregistered driver_options type',
                workflows_pb2.BuildConfig(
                    name='base', driver_options=_UNREGISTERED_OPTIONS
                ),
                workflows_pb2.BuildConfig(
                    name='other',
                    mixin=True,
                    driver_options=_UNREGISTERED_OPTIONS,
                ),
                'driver_options type `pw.test.Unregistered` is not registered',
            ),
        ]
        for name, base, mixin, message in cases:
            with (
                self.subTest(name),
                self.assertRaisesRegex(ValueError, re.escape(message)),
            ):
                merge_build_configs(base, [mixin])


if __name__ == '__main__':
    unittest.main()
