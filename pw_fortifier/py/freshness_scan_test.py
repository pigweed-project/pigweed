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
"""Tests for freshness_scan binary."""
# pylint: disable=protected-access,missing-class-docstring,missing-function-docstring,no-self-use

from datetime import date
import io
import os
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.freshness_scan import main
from pw_fortifier.package_scanner import PackageVersion, FreshnessScanResult


class TestFreshnessScan(unittest.TestCase):

    def test_main(self):

        # 1. Fresh
        res_fresh = FreshnessScanResult(
            package='fresh-pkg',
            source='MODULE.bazel',
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 6, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 6, 1)),
            tier=1,
            owner='owner@google.com',
        )

        # 2. Stale (overdue by 12 days)
        res_stale = FreshnessScanResult(
            package='stale-pkg',
            source='MODULE.bazel',
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 5, 20)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=1,
            owner='owner@google.com',
        )

        with (
            patch('sys.argv', ['freshness_scan']),
            patch(
                'pw_fortifier.package_scanner.'
                'PackageScannerRegistry.scan_all',
                return_value=iter([res_fresh, res_stale]),
            ),
            patch('sys.stdout', new_callable=io.StringIO) as mock_stdout,
        ):
            main()
            output = mock_stdout.getvalue()

        lines = output.strip().splitlines()

        self.assertEqual(len(lines), 3)
        expected_age = (date.today() - date(2026, 6, 1)).days
        # Check header
        self.assertEqual(
            lines[0],
            'package,tier,tier_desc,type,freshness,'
            'freshness_desc,current_ver,current_ts,'
            'earliest_ver,earliest_ts,earliest_age,owner,source',
        )
        # Check data
        self.assertEqual(
            lines[1],
            f'fresh-pkg,1,used in build,bazel_dep,100,Fresh,'
            f'1.0.0,2026-06-01,1.0.0,2026-06-01,{expected_age},'
            f'owner@google.com,MODULE.bazel',
        )
        self.assertEqual(
            lines[2],
            f'stale-pkg,1,used in build,bazel_dep,-12,12 days overdue,'
            f'1.0.0,2026-05-20,2.0.0,2026-06-01,{expected_age},'
            f'owner@google.com,MODULE.bazel',
        )

    def test_output_file(self):
        res_fresh = FreshnessScanResult(
            package='fresh-pkg',
            source='MODULE.bazel',
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 6, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 6, 1)),
            tier=1,
            owner='owner@google.com',
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = os.path.join(tmpdir, 'output.csv')
            with (
                patch('sys.argv', ['freshness_scan', '--output', out_path]),
                patch(
                    'pw_fortifier.package_scanner.'
                    'PackageScannerRegistry.scan_all',
                    return_value=iter([res_fresh]),
                ),
            ):
                main()

            with open(out_path, 'r', encoding='utf-8') as f:
                content = f.read()

        lines = content.strip().splitlines()
        self.assertEqual(len(lines), 2)
        expected_age = (date.today() - date(2026, 6, 1)).days
        # Check header
        self.assertEqual(
            lines[0],
            'package,tier,tier_desc,type,freshness,'
            'freshness_desc,current_ver,current_ts,'
            'earliest_ver,earliest_ts,earliest_age,owner,source',
        )
        # Check data
        self.assertEqual(
            lines[1],
            f'fresh-pkg,1,used in build,bazel_dep,100,Fresh,'
            f'1.0.0,2026-06-01,1.0.0,2026-06-01,{expected_age},'
            f'owner@google.com,MODULE.bazel',
        )


if __name__ == '__main__':
    unittest.main()
