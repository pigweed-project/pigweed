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
"""Tests the python_packages module."""

import importlib.metadata
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from packaging.requirements import Requirement
from pw_env_setup import python_packages


def _make_mock_dist(
    name: str,
    version: str = '1.0.0',
    path: str = '/site-packages/pkg-1.0.0.dist-info',
    direct_url: str | None = None,
    requires: list[str] | None = None,
) -> mock.MagicMock:
    """Creates a mock importlib.metadata.Distribution for testing."""
    dist = mock.MagicMock(spec=importlib.metadata.Distribution)
    dist.name = name
    dist.version = version
    dist.requires = requires
    dist._path = Path(path)  # pylint: disable=protected-access
    dist.read_text.side_effect = lambda filename: (
        direct_url if filename == 'direct_url.json' else None
    )
    return dist


class TestPythonPackages(unittest.TestCase):
    """Tests the python_packages module."""

    def setUp(self):
        self.existing_pkgs_minus_packaging = '\n'.join(
            pkg
            for pkg in python_packages._installed_packages()  # pylint: disable=protected-access
            if not pkg.startswith('packaging==')
        )
        self.temp_dir = tempfile.TemporaryDirectory()
        self.temp_path = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_list(self):
        # pylint: disable=protected-access
        pkgs = list(python_packages._installed_packages())
        # pylint: enable=protected-access
        packaging_version = importlib.metadata.version('packaging')

        self.assertIn(f'packaging=={packaging_version}', pkgs)
        self.assertNotIn('pw-foo', pkgs)

    def test_is_dist_info(self):
        """Tests _is_dist_info filtering of editable and .egg-info packages."""
        # pylint: disable=protected-access
        regular_dist = _make_mock_dist('foo')
        self.assertTrue(python_packages._is_dist_info(regular_dist))

        egg_info_dist = _make_mock_dist(
            'pw-foo', path='/site-packages/pw_foo.egg-info'
        )
        self.assertFalse(python_packages._is_dist_info(egg_info_dist))

        editable_dist = _make_mock_dist(
            'pw-bar',
            path='/site-packages/pw_bar-0.0.1.dist-info',
            direct_url=(
                '{"url": "file:///src/pw_bar", "dir_info": {"editable": true}}'
            ),
        )
        self.assertFalse(python_packages._is_dist_info(editable_dist))

        non_editable_direct_url_dist = _make_mock_dist(
            'bar',
            direct_url=(
                '{"url": "file:///src/bar", "dir_info": {"editable": false}}'
            ),
        )
        self.assertTrue(
            python_packages._is_dist_info(non_editable_direct_url_dist)
        )

        null_dir_info_dist = _make_mock_dist(
            'baz',
            direct_url=(
                '{"url": "https://example.com/baz.whl", "dir_info": null}'
            ),
        )
        self.assertTrue(python_packages._is_dist_info(null_dir_info_dist))

        malformed_direct_url_dist = _make_mock_dist(
            'broken',
            direct_url='{not valid json',
        )
        with self.assertRaises(json.JSONDecodeError):
            python_packages._is_dist_info(malformed_direct_url_dist)
        # pylint: enable=protected-access

    @mock.patch('importlib.metadata.distributions')
    def test_installed_packages_deduplicates_and_excludes_editable(
        self, distributions_mock
    ):
        """Tests deduplication and editable filtering across sys.path."""
        distributions_mock.return_value = [
            # Editable package earlier on sys.path shadows wheel later on
            # sys.path.
            _make_mock_dist(
                'pw-foo',
                version='0.1.0',
                direct_url='{"dir_info": {"editable": true}}',
            ),
            _make_mock_dist('pw_foo', version='0.1.0'),
            # First distribution wins when duplicated across sys.path entries.
            _make_mock_dist('My_Pkg', version='2.0.0'),
            _make_mock_dist('my-pkg', version='1.0.0'),
        ]
        # pylint: disable=protected-access
        self.assertEqual(
            list(python_packages._installed_packages()),
            ['my-pkg==2.0.0'],
        )
        # pylint: enable=protected-access

    def test_load_requirements_lines(self):
        """Tests stripping comments, hashes, and continuations from files."""
        other_file = self.temp_path / 'other_constraints.txt'
        other_file.write_text('other==0.5\n', encoding='utf-8')
        req_file = self.temp_path / 'requirements.txt'
        req_file.write_text(
            '\n'.join(
                [
                    '-c ./other_constraints.txt',
                    '# top-level comment',
                    '   # indented comment',
                    '',
                    '   ',
                    'foo==1.0  # inline comment',
                    'bar==2.0 \\',
                    '    --hash=sha256:abcdef0123456789 \\',
                    'baz==3.0 \\   ',
                ]
            ),
            encoding='utf-8',
        )
        # pylint: disable=protected-access
        self.assertEqual(
            list(python_packages._load_requirements_lines(req_file)),
            ['foo==1.0', 'bar==2.0', 'baz==3.0'],
        )
        self.assertEqual(
            list(
                python_packages._load_requirements_lines(
                    req_file, follow_constraints=True
                )
            ),
            ['other==0.5', 'foo==1.0', 'bar==2.0', 'baz==3.0'],
        )
        # pylint: enable=protected-access

    @mock.patch('pw_env_setup.python_packages._stderr')
    def test_diff_removed(self, stderr_mock):
        expected = 'foo==1.0\nbar==2.0\nbaz==3.0\n'
        expected_file = self.temp_path / 'test_diff_removed_expected'
        expected_file.write_text(expected, encoding='utf-8')

        # Removed packages should trigger a failure.
        self.assertEqual(-1, python_packages.diff(expected_file))

        stderr_mock.assert_any_call('Removed packages')
        stderr_mock.assert_any_call('  foo==1.0')
        stderr_mock.assert_any_call('  bar==2.0')
        stderr_mock.assert_any_call('  baz==3.0')

    @mock.patch('pw_env_setup.python_packages._stderr')
    def test_diff_updated(self, stderr_mock):
        expected = 'packaging>=0.0.1\n' + self.existing_pkgs_minus_packaging
        expected_file = self.temp_path / 'test_diff_updated_expected'
        expected_file.write_text(expected, encoding='utf-8')

        packaging_version = importlib.metadata.version('packaging')

        # Updated packages should trigger a failure.
        self.assertEqual(-1, python_packages.diff(expected_file))

        stderr_mock.assert_any_call('Updated packages')
        stderr_mock.assert_any_call(
            f'  packaging=={packaging_version} (from packaging>=0.0.1)'
        )

    @mock.patch('pw_env_setup.python_packages._stderr')
    def test_diff_new(self, stderr_mock):
        expected = self.existing_pkgs_minus_packaging
        expected_file = self.temp_path / 'test_diff_new_expected'
        expected_file.write_text(expected, encoding='utf-8')

        packaging_version = importlib.metadata.version('packaging')

        # New packages should trigger a failure.
        self.assertEqual(-1, python_packages.diff(expected_file))

        stderr_mock.assert_any_call('New packages')
        stderr_mock.assert_any_call(f'  packaging=={packaging_version}')
        stderr_mock.assert_any_call("Package versions don't match!")

    @mock.patch('importlib.metadata.distributions')
    def test_ls_preserves_marker_without_crashing_on_excluded_package(
        self, distributions_mock
    ):
        """Tests ls() does not fail on marked excluded packages."""
        distributions_mock.return_value = [
            _make_mock_dist('setuptools', version='68.0.0'),
            _make_mock_dist('foo', version='1.0.0'),
        ]
        out_file = self.temp_path / 'requirements.txt'
        out_file.write_text(
            f'setuptools==68.0.0 ; sys_platform == "{sys.platform}"\n'
            f'foo==1.0.0 ; sys_platform == "{sys.platform}"\n',
            encoding='utf-8',
        )
        self.assertEqual(0, python_packages.ls(out_file))
        self.assertEqual(
            out_file.read_text(encoding='utf-8'),
            f'foo==1.0.0; sys_platform == "{sys.platform}"\n',
        )

    @mock.patch('pw_env_setup.python_packages._stderr')
    @mock.patch('importlib.metadata.distributions')
    def test_diff_ignored_transitive_deps(
        self, distributions_mock, stderr_mock
    ):
        """Tests ignored transitive deps do not mask new packages."""
        expected_dist = _make_mock_dist('expected-pkg', version='1.0.0')
        ignored_dist = _make_mock_dist(
            'ignored-pkg',
            version='2.0.0',
            requires=['transitive-dep==3.0.0 ; extra == "ext"'],
        )
        transitive_dist = _make_mock_dist('transitive-dep', version='3.0.0')
        unexpected_dist = _make_mock_dist('unexpected-pkg', version='4.0.0')

        dists = {
            'expected-pkg': expected_dist,
            'ignored-pkg': ignored_dist,
            'transitive-dep': transitive_dist,
            'unexpected-pkg': unexpected_dist,
        }
        distributions_mock.return_value = list(dists.values())

        expected_file = self.temp_path / 'expected.txt'
        expected_file.write_text('expected-pkg==1.0.0\n', encoding='utf-8')

        ignored_file = self.temp_path / 'ignored.txt'
        ignored_file.write_text('ignored-pkg[ext]==2.0.0\n', encoding='utf-8')

        self.assertEqual(
            -1,
            python_packages.diff(
                expected_file, ignore_requirements_file=[ignored_file]
            ),
        )
        stderr_mock.assert_any_call('New packages')
        stderr_mock.assert_any_call('  unexpected-pkg==4.0.0')
        for call in stderr_mock.call_args_list:
            self.assertNotIn('ignored-pkg', call.args[0])
            self.assertNotIn('transitive-dep', call.args[0])

    @mock.patch('pw_env_setup.python_packages._stderr')
    @mock.patch('importlib.metadata.distributions')
    def test_diff_ignored_unpinned_requirement(
        self, distributions_mock, stderr_mock
    ):
        """Tests unpinned and range-specified ignored requirements."""
        unpinned_dist = _make_mock_dist('unpinned-pkg', version='1.0.0')
        range_dist = _make_mock_dist('range-pkg', version='2.5.0')
        distributions_mock.return_value = [unpinned_dist, range_dist]

        expected_file = self.temp_path / 'expected.txt'
        expected_file.write_text('', encoding='utf-8')
        ignored_file = self.temp_path / 'ignored.txt'
        ignored_file.write_text(
            'unpinned-pkg\nrange-pkg>=2.0\n', encoding='utf-8'
        )

        self.assertEqual(
            0,
            python_packages.diff(
                expected_file, ignore_requirements_file=[ignored_file]
            ),
        )
        for call in stderr_mock.call_args_list:
            self.assertNotIn('unpinned-pkg', call.args[0])
            self.assertNotIn('range-pkg', call.args[0])

    @mock.patch('pw_env_setup.python_packages._working_set')
    def test_get_ignored_requirements_circular_dependencies(
        self, working_set_mock
    ):
        """Tests circular dependencies in ignored requirements terminate."""
        pkg_a = _make_mock_dist('pkg-a', requires=['pkg-b==1.0.0'])
        pkg_b = _make_mock_dist('pkg-b', requires=['pkg-a==1.0.0'])
        working_set_mock.return_value = {'pkg-a': pkg_a, 'pkg-b': pkg_b}

        # pylint: disable=protected-access
        _, ignored_trans = python_packages._get_ignored_requirements(
            expected_requirements=frozenset(),
            ignored_lines={'pkg-a==1.0.0'},
        )
        # pylint: enable=protected-access
        self.assertIn(Requirement('pkg-b==1.0.0'), ignored_trans)

    @mock.patch('pw_env_setup.python_packages._stderr')
    def test_diff_marker_semicolon_space(self, stderr_mock):
        """Tests space before marker semicolon does not warn on reformatting."""
        expected = (
            'tomli==2.0.1 ; python_version < "1.0"\n'
            + 'packaging=='
            + importlib.metadata.version('packaging')
            + '\n'
            + self.existing_pkgs_minus_packaging
        )
        expected_file = self.temp_path / 'test_diff_marker_semicolon_expected'
        expected_file.write_text(expected, encoding='utf-8')

        self.assertEqual(0, python_packages.diff(expected_file))
        for call in stderr_mock.call_args_list:
            for arg in call.args:
                self.assertNotIn('tomli==2.0.1', arg)

    @mock.patch('pw_env_setup.python_packages._stderr')
    def test_diff_reformatted_non_canonical_name(self, stderr_mock):
        """Tests non-canonical package names are flagged for reformatting."""
        expected = (
            'foo_bar==1.0.0; python_version < "1.0"\n'
            + 'packaging=='
            + importlib.metadata.version('packaging')
            + '\n'
            + self.existing_pkgs_minus_packaging
        )
        expected_file = self.temp_path / 'test_diff_reformatted_expected'
        expected_file.write_text(expected, encoding='utf-8')

        self.assertEqual(0, python_packages.diff(expected_file))
        stderr_mock.assert_any_call('Requirements that need reformatting:')
        stderr_mock.assert_any_call(
            '  "foo_bar==1.0.0; python_version < "1.0""'
        )
        stderr_mock.assert_any_call(
            '  "foo-bar==1.0.0; python_version < "1.0""'
        )

    @mock.patch('pw_env_setup.python_packages.ls', return_value=0)
    def test_main_ls_alias(self, ls_mock):
        """Tests the ls command alias dispatches to ls()."""
        with mock.patch.object(sys, 'argv', ['python_packages', 'ls']):
            self.assertEqual(0, python_packages.main())
        ls_mock.assert_called_once_with(output_file=None)


if __name__ == '__main__':
    unittest.main()
