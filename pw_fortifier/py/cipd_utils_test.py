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
"""Tests for cipd_utils in pw_fortifier."""
# pylint: disable=protected-access

from datetime import date
import subprocess
import unittest
from unittest.mock import AsyncMock, patch

from pw_fortifier.cipd_utils import (
    CipdPackageSet,
    _discover_platforms,
    _get_package_version,
    _get_package_versions,
    _has_version,
    _run_cipd,
    _run_cipd_describe,
    _run_cipd_instances,
    _split_platform,
)
from pw_fortifier.freshness_result import PackageVersion, TIER1_TOOLCHAIN


class TestCipdUtils(unittest.IsolatedAsyncioTestCase):
    """Tests for cipd_utils."""

    def test_split_platform(self) -> None:
        """Test splitting platform template and platform suffix."""
        self.assertEqual(
            _split_platform('fuchsia/third_party/clang/${platform}'),
            ('fuchsia/third_party/clang', '${platform}'),
        )
        self.assertEqual(
            _split_platform('fuchsia/third_party/clang/${os}-${arch}'),
            ('fuchsia/third_party/clang', '${os}-${arch}'),
        )
        self.assertEqual(
            _split_platform('fuchsia/third_party/clang/linux-amd64'),
            ('fuchsia/third_party/clang', 'linux-amd64'),
        )
        self.assertEqual(
            _split_platform('fuchsia/third_party/3pp/bloaty/common'),
            ('fuchsia/third_party/3pp/bloaty/common', None),
        )
        self.assertEqual(
            _split_platform('fuchsia/third_party/sysroot/bionic'),
            ('fuchsia/third_party/sysroot/bionic', None),
        )
        self.assertEqual(
            _split_platform('fuchsia/third_party/foo/some-tool'),
            ('fuchsia/third_party/foo/some-tool', None),
        )
        self.assertEqual(
            _split_platform('gn/gn'),
            ('gn/gn', None),
        )

    @patch('subprocess.run')
    async def test_run_cipd_success(self, mock_run: AsyncMock) -> None:
        """Test _run_cipd returns lines on success."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=['cipd', 'ls'],
            returncode=0,
            stdout='line1\nline2\n',
            stderr='',
        )

        result = await _run_cipd(['ls'])
        self.assertEqual(result, ['line1', 'line2'])

    @patch('subprocess.run')
    async def test_run_cipd_file_not_found(self, mock_run: AsyncMock) -> None:
        """Test _run_cipd raises FileNotFoundError when cipd is missing."""
        mock_run.side_effect = FileNotFoundError('cipd not found')

        with self.assertRaises(FileNotFoundError) as ctx:
            await _run_cipd(['ls'])
        self.assertIn('`cipd` command not found', str(ctx.exception))
        self.assertIn('activate the Pigweed environment', str(ctx.exception))

    @patch('subprocess.run')
    async def test_run_cipd_called_process_error(
        self, mock_run: AsyncMock
    ) -> None:
        """Test _run_cipd raises RuntimeError on non-zero exit."""
        mock_run.side_effect = subprocess.CalledProcessError(1, ['cipd', 'ls'])

        with self.assertRaises(RuntimeError):
            await _run_cipd(['ls'])

    @patch('pw_fortifier.cipd_utils._run_cipd')
    async def test_run_cipd_describe_parsing(
        self, mock_run_cipd: AsyncMock
    ) -> None:
        """Test parsing cipd describe output."""
        mock_output = [
            'Package:       pigweed/third_party/doxygen/linux-amd64',
            (
                'Instance:      pigweed/third_party/doxygen/linux-amd64:'
                'git_revision:5d15657a55555e6181a7830a5c723af75e7577e2'
            ),
            'Registered by: user@google.com',
            'Registered at: 2026-06-01 12:00:00 -0700 MST',
            'Refs:',
            '  latest',
            '  prod',
            'Tags:',
            '  git_repository:https://github.com/doxygen/doxygen',
            '  git_revision:5d15657a55555e6181a7830a5c723af75e7577e2',
            '  version:1.9.4',
            'Metadata:',
            '  version:1.9.4',
        ]
        mock_run_cipd.return_value = mock_output

        result = await _run_cipd_describe('pkg', 'ref')

        expected = {
            'Package': 'pigweed/third_party/doxygen/linux-amd64',
            'Instance': (
                'pigweed/third_party/doxygen/linux-amd64:'
                'git_revision:5d15657a55555e6181a7830a5c723af75e7577e2'
            ),
            'Registered by': 'user@google.com',
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Refs': {'latest': '', 'prod': ''},
            'Tags': {
                'git_repository': 'https://github.com/doxygen/doxygen',
                'git_revision': '5d15657a55555e6181a7830a5c723af75e7577e2',
                'version': '1.9.4',
            },
            'Metadata': {'version': '1.9.4'},
        }

        self.assertEqual(result, expected)

    @patch('pw_fortifier.cipd_utils._run_cipd')
    async def test_run_cipd_describe_failure(
        self, mock_run_cipd: AsyncMock
    ) -> None:
        """Test _run_cipd_describe when cipd command fails."""
        mock_run_cipd.side_effect = RuntimeError('cipd failed')

        with self.assertRaises(RuntimeError):
            await _run_cipd_describe('pkg', 'ref')

    @patch('pw_fortifier.cipd_utils._run_cipd')
    async def test_run_cipd_instances_parsing(
        self, mock_run_cipd: AsyncMock
    ) -> None:
        """Test parsing cipd instances output."""
        mock_output = [
            (
                'Instance ID                                   '
                'Registered by        Registered at'
            ),
            (
                '----------------------------------------'
                '-----------------------------------------'
            ),
            (
                'git_revision:ninja_latest_hash                '
                'user@google.com      2026-06-08 12:00:00 -0700 MST'
            ),
            (
                'git_revision:ninja_mid_hash                   '
                'user@google.com      2026-06-06 12:00:00 -0700 MST'
            ),
            (
                'git_revision:ninja123                        '
                'user@google.com      2026-06-05 12:00:00 -0700 MST'
            ),
            (
                '...                                          '
                '...                  ...'
            ),
        ]
        mock_run_cipd.return_value = mock_output

        result = [x async for x in _run_cipd_instances('pkg')]

        expected = [
            'git_revision:ninja_latest_hash',
            'git_revision:ninja_mid_hash',
            'git_revision:ninja123',
        ]
        self.assertEqual(result, expected)

    @patch('pw_fortifier.cipd_utils._run_cipd')
    async def test_run_cipd_instances_failure(
        self, mock_run_cipd: AsyncMock
    ) -> None:
        """Test _run_cipd_instances when cipd command fails."""
        mock_run_cipd.side_effect = RuntimeError('cipd failed')

        with self.assertRaises(RuntimeError):
            _ = [x async for x in _run_cipd_instances('pkg')]

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_has_version(self, mock_describe: AsyncMock) -> None:
        """Test _has_version returns True on success and False on error."""
        mock_describe.return_value = {'Tags': {'version': '1.0.0'}}
        self.assertTrue(await _has_version('pkg', '1.0.0'))

        mock_describe.side_effect = RuntimeError('describe failed')
        self.assertFalse(await _has_version('pkg', '1.0.0'))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_has_version_git_revision_fallback(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _has_version falls back to git_revision prefix."""

        async def fake_describe(_pkg, ref):
            if ref == 'git_revision:5d15657a':
                return {'Tags': {'git_revision': '5d15657a'}}
            raise RuntimeError('not found')

        mock_describe.side_effect = fake_describe
        self.assertTrue(await _has_version('pkg', '5d15657a'))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_get_package_version_success(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _get_package_version parses version and date."""
        mock_describe.return_value = {
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Tags': {'version': '1.2.3'},
        }

        pv = await _get_package_version('pkg', 'ref')
        assert pv is not None
        self.assertEqual(pv.version, '1.2.3')
        self.assertEqual(pv.timestamp, date(2026, 6, 1))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_get_package_version_git_revision(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _get_package_version preserves git_revision: prefix."""
        mock_describe.return_value = {
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Tags': {
                'git_revision': '5d15657a55555e6181a7830a5c723af75e7577e2'
            },
        }

        pv = await _get_package_version('pkg', 'ref')
        assert pv is not None
        self.assertEqual(
            pv.version, 'git_revision:5d15657a55555e6181a7830a5c723af75e7577e2'
        )
        self.assertEqual(pv.timestamp, date(2026, 6, 1))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_get_package_version_git_revisions(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _get_package_version preserves git_revisions: prefix."""
        mock_describe.return_value = {
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Tags': {'git_revisions': 'hash1,hash2'},
        }

        pv = await _get_package_version('pkg', 'ref')
        assert pv is not None
        self.assertEqual(pv.version, 'git_revisions:hash1,hash2')
        self.assertEqual(pv.timestamp, date(2026, 6, 1))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_get_package_version_g3_revision(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _get_package_version preserves g3-revision: prefix."""
        mock_describe.return_value = {
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Tags': {
                'g3-revision': (
                    'fuchsia.infra.coverage.upload_clients_20260827_RC00'
                )
            },
        }

        pv = await _get_package_version('pkg', 'ref')
        assert pv is not None
        self.assertEqual(
            pv.version,
            'g3-revision:fuchsia.infra.coverage.upload_clients_20260827_RC00',
        )
        self.assertEqual(pv.timestamp, date(2026, 6, 1))

    @patch('pw_fortifier.cipd_utils._run_cipd_describe')
    async def test_get_package_version_none_when_missing(
        self, mock_describe: AsyncMock
    ) -> None:
        """Test _get_package_version returns None when version is missing."""
        mock_describe.return_value = {
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Tags': {'other_tag': 'foo'},
        }

        pv = await _get_package_version('pkg', 'ref')
        self.assertIsNone(pv)

    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._run_cipd_instances')
    async def test_get_package_versions(
        self,
        mock_instances: AsyncMock,
        mock_get_ver: AsyncMock,
    ) -> None:
        """Test _get_package_versions gets all valid versions."""

        async def fake_instances(_pkg):
            yield 'inst1'
            yield 'inst2'

        mock_instances.side_effect = fake_instances
        mock_get_ver.side_effect = [
            PackageVersion('1.0.0', date(2026, 1, 1)),
            PackageVersion('2.0.0', date(2026, 2, 1)),
        ]

        versions = [v async for v in _get_package_versions('pkg')]
        self.assertEqual(len(versions), 2)
        self.assertEqual(versions[0].version, '1.0.0')
        self.assertEqual(versions[1].version, '2.0.0')

    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._run_cipd_instances')
    async def test_get_package_versions_unrecognized_version_skipped(
        self,
        mock_instances: AsyncMock,
        mock_get_ver: AsyncMock,
    ) -> None:
        """Test _get_package_versions skips instances when version is None."""

        async def fake_instances(_pkg):
            yield 'inst1'

        mock_instances.side_effect = fake_instances
        mock_get_ver.return_value = None

        versions = [v async for v in _get_package_versions('pkg')]
        self.assertEqual(versions, [])

    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._run_cipd_instances')
    async def test_get_package_versions_early_termination(
        self,
        mock_instances: AsyncMock,
        mock_get_ver: AsyncMock,
    ) -> None:
        """Test _get_package_versions stops when until version is reached."""

        async def fake_instances(_pkg):
            yield 'inst_newest'
            yield 'inst_current'
            yield 'inst_older'

        mock_instances.side_effect = fake_instances
        mock_get_ver.side_effect = [
            PackageVersion('3.0.0', date(2026, 3, 1)),
            PackageVersion('2.0.0', date(2026, 2, 1)),
            PackageVersion('1.0.0', date(2026, 1, 1)),
        ]

        current = PackageVersion('2.0.0', date(2026, 2, 1))
        versions = [v async for v in _get_package_versions('pkg', current)]
        self.assertEqual(len(versions), 2)
        self.assertEqual(versions[0].version, '3.0.0')
        self.assertEqual(versions[1].version, '2.0.0')
        self.assertEqual(mock_get_ver.call_count, 2)

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 1, 10))
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    async def test_cipd_package_set(
        self,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
    ) -> None:
        """Test CipdPackageSet generates freshness results."""
        mock_get_version.return_value = PackageVersion(
            '1.0.0', date(2026, 1, 1)
        )

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('1.0.0', date(2026, 1, 1))
            yield PackageVersion('2.0.0', date(2026, 1, 5))

        mock_get_versions.side_effect = fake_get_versions

        pkg_set = CipdPackageSet('MODULE.bazel', 'bazel_cipd')
        await pkg_set.add('infra/3p/pkg', 'tag1', TIER1_TOOLCHAIN)

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].package, 'infra/3p/pkg')
        self.assertEqual(results[0].earliest.version, '1.0.0')

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 6, 10))
    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    @patch('pw_fortifier.cipd_utils._has_version')
    async def test_cipd_package_set_missing_platform_candidate(
        self,
        mock_has_version: AsyncMock,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
        mock_run_cipd: AsyncMock,
    ) -> None:
        """Test candidate versions require instances on all platforms."""
        mock_run_cipd.return_value = [
            'fuchsia/third_party/qemu/linux-amd64',
            'fuchsia/third_party/qemu/mac-arm64',
        ]

        async def fake_has_version(pkg: str, ver: str) -> bool:
            if 'mac-arm64' in pkg and ver == '1.2.0':
                return False
            return True

        mock_has_version.side_effect = fake_has_version
        mock_get_version.return_value = PackageVersion(
            '1.0.0', date(2026, 1, 1)
        )

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('1.0.0', date(2026, 1, 1))
            yield PackageVersion('1.1.0', date(2026, 5, 1))
            yield PackageVersion('1.2.0', date(2026, 6, 8))

        mock_get_versions.side_effect = fake_get_versions

        pkg_set = CipdPackageSet('pigweed.json', 'cipd_setup')
        await pkg_set.add(
            'fuchsia/third_party/qemu/${platform}',
            '1.0.0',
            TIER1_TOOLCHAIN,
            platforms=['linux-amd64', 'mac-arm64'],
        )

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].package, 'fuchsia/third_party/qemu')
        self.assertEqual(results[0].current.version, '1.0.0')
        self.assertEqual(results[0].earliest.version, '1.1.0')

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 6, 10))
    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    @patch('pw_fortifier.cipd_utils._has_version')
    async def test_cipd_package_set_implicit_platforms(
        self,
        mock_has_version: AsyncMock,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
        mock_run_cipd: AsyncMock,
    ) -> None:
        """Test platforms without explicit list filter to current version."""
        mock_run_cipd.return_value = [
            'fuchsia/third_party/clang/linux-amd64',
            'fuchsia/third_party/clang/mac-arm64',
            'fuchsia/third_party/clang/linux-riscv64',
        ]

        async def fake_has_version(pkg: str, _ref: str) -> bool:
            if 'linux-riscv64' in pkg:
                return False
            return True

        mock_has_version.side_effect = fake_has_version
        mock_get_version.return_value = PackageVersion('tag1', date(2026, 1, 1))

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('tag1', date(2026, 1, 1))
            yield PackageVersion('tag2', date(2026, 5, 1))

        mock_get_versions.side_effect = fake_get_versions

        pkg_set = CipdPackageSet('MODULE.bazel', 'bazel_cipd')
        await pkg_set.add(
            'fuchsia/third_party/clang/${platform}',
            'tag1',
            TIER1_TOOLCHAIN,
        )

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].package, 'fuchsia/third_party/clang')
        self.assertEqual(results[0].current.version, 'tag1')
        self.assertEqual(results[0].earliest.version, 'tag2')

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 6, 10))
    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    @patch('pw_fortifier.cipd_utils._has_version')
    async def test_cipd_package_set_no_candidates_fallback(
        self,
        mock_has_version: AsyncMock,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
        _mock_run_cipd: AsyncMock,
    ) -> None:
        """Test fallback to current when no versions match all platforms."""
        mock_get_version.return_value = PackageVersion(
            '1.0.0', date(2026, 1, 1)
        )

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('1.0.0', date(2026, 1, 1))
            yield PackageVersion('1.1.0', date(2026, 5, 1))

        mock_get_versions.side_effect = fake_get_versions
        mock_has_version.return_value = False

        pkg_set = CipdPackageSet('pigweed.json', 'cipd_setup')
        await pkg_set.add(
            'fuchsia/third_party/qemu/${platform}',
            '1.0.0',
            TIER1_TOOLCHAIN,
            platforms=['linux-amd64', 'mac-arm64'],
        )

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].package, 'fuchsia/third_party/qemu')
        self.assertEqual(results[0].current.version, '1.0.0')
        self.assertEqual(results[0].earliest.version, '1.0.0')

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 6, 10))
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    async def test_cipd_package_set_non_platformed_ignores_platforms(
        self,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
    ) -> None:
        """Test non-platformed package ignores platforms arg."""
        mock_get_version.return_value = PackageVersion(
            'git_revision:702eb965', date(2026, 1, 1)
        )

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('git_revision:702eb965', date(2026, 1, 1))
            yield PackageVersion('git_revision:newer1234', date(2026, 5, 1))

        mock_get_versions.side_effect = fake_get_versions

        pkg_set = CipdPackageSet('pigweed.json', 'cipd_setup')
        await pkg_set.add(
            'fuchsia/third_party/sysroot/bionic',
            'git_revision:702eb965',
            TIER1_TOOLCHAIN,
            platforms=['linux-amd64', 'linux-arm64'],
        )

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(
            results[0].package, 'fuchsia/third_party/sysroot/bionic'
        )
        self.assertEqual(results[0].current.version, 'git_revision:702eb965')
        self.assertEqual(results[0].earliest.version, 'git_revision:newer1234')
        mock_get_version.assert_called_once_with(
            'fuchsia/third_party/sysroot/bionic', 'git_revision:702eb965'
        )

    @patch('pw_fortifier.package_analyzer.DATE', date(2026, 6, 10))
    @patch('pw_fortifier.cipd_utils._get_package_version')
    @patch('pw_fortifier.cipd_utils._get_package_versions')
    async def test_cipd_package_set_fixed_platform_path_with_explicit_platforms(
        self,
        mock_get_versions: AsyncMock,
        mock_get_version: AsyncMock,
    ) -> None:
        """Test fixed path ending in platform with explicit platforms list."""
        mock_get_version.return_value = PackageVersion(
            'version:1.9.6-1', date(2026, 1, 1)
        )

        async def fake_get_versions(_pkg, _until=None):
            yield PackageVersion('version:1.9.6-1', date(2026, 1, 1))

        mock_get_versions.side_effect = fake_get_versions

        pkg_set = CipdPackageSet('doxygen.json', 'cipd_setup')
        await pkg_set.add(
            'pigweed/third_party/doxygen/mac-amd64',
            'version:1.9.6-1',
            TIER1_TOOLCHAIN,
            platforms=['mac-arm64'],
        )

        scanned: set[str] = set()
        results = [res async for res in pkg_set.generate_results(scanned)]
        self.assertEqual(len(results), 1)
        self.assertEqual(
            results[0].package, 'pigweed/third_party/doxygen/mac-amd64'
        )
        self.assertEqual(results[0].current.version, 'version:1.9.6-1')
        mock_get_version.assert_called_once_with(
            'pigweed/third_party/doxygen/mac-amd64', 'version:1.9.6-1'
        )

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('pw_fortifier.cipd_utils._has_version')
    async def test_discover_platforms(
        self,
        mock_has_version: AsyncMock,
        mock_run_cipd: AsyncMock,
    ) -> None:
        """Test discovering platforms matching version."""
        mock_run_cipd.return_value = [
            'fuchsia/third_party/clang/linux-amd64',
            'fuchsia/third_party/clang/mac-amd64',
            'fuchsia/third_party/clang/mac-arm64',
        ]

        async def fake_has_version(pkg: str, _ver: str) -> bool:
            return 'mac-arm64' not in pkg

        mock_has_version.side_effect = fake_has_version

        platforms = await _discover_platforms(
            'fuchsia/third_party/clang', '123'
        )
        self.assertEqual(platforms, ['linux-amd64', 'mac-amd64'])


if __name__ == '__main__':
    unittest.main()
