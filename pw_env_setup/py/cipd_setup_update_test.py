# Copyright 2023 The Pigweed Authors
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
"""Tests for cipd_setup.update logic."""

import importlib.resources
import operator
from pathlib import Path
import unittest

from parameterized import parameterized  # type: ignore

from pw_env_setup.cipd_setup.update import (
    all_package_files,
    deduplicate_packages,
)


class TestCipdSetupUpdate(unittest.TestCase):
    """Tests for cipd_setup.update logic."""

    maxDiff = None

    @parameterized.expand(
        [
            (
                'overriden Python',
                [
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@12.2.mpacbti-rel1.1'],
                        'subdir': 'arm',
                    },
                    {
                        'path': 'infra/3pp/tools/cpython3/${platform}',
                        'tags': ['version:2@3.8.10.chromium.24'],
                        'subdir': 'arm/python',
                        'original_subdir': 'python',
                    },
                    # Python 3.11.3
                    {
                        'path': 'infra/3pp/tools/cpython3/${platform}',
                        'tags': ['version:2@3.11.3.chromium.29'],
                        'subdir': 'python',
                    },
                    # Duplicate Python, different version 3.11.4
                    # This should take precedence.
                    {
                        'path': 'infra/3pp/tools/cpython3/${platform}',
                        'tags': ['version:2@3.11.4.chromium.29'],
                        'subdir': 'python',
                    },
                ],
                [
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@12.2.mpacbti-rel1.1'],
                        'subdir': 'arm',
                    },
                    {
                        'path': 'infra/3pp/tools/cpython3/${platform}',
                        'tags': ['version:2@3.8.10.chromium.24'],
                        'subdir': 'arm/python',
                        'original_subdir': 'python',
                    },
                    {
                        'path': 'infra/3pp/tools/cpython3/${platform}',
                        'tags': ['version:2@3.11.4.chromium.29'],
                        'subdir': 'python',
                    },
                ],
            ),
            (
                'duplicate package in a different subdir',
                [
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@12.2.mpacbti-rel1.1'],
                        'subdir': 'arm',
                    },
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@10.3-2021.10.1'],
                        'subdir': 'another_arm',
                    },
                ],
                [
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@10.3-2021.10.1'],
                        'subdir': 'another_arm',
                    },
                ],
            ),
            (
                'duplicate package in the same subdir',
                [
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@12.2.mpacbti-rel1.1'],
                        'subdir': 'arm',
                    },
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@10.3-2021.10.1'],
                        'subdir': 'arm',
                    },
                ],
                [
                    # The second older version takes precedence
                    {
                        'path': 'fuchsia/third_party/armgcc/${platform}',
                        'tags': ['version:2@10.3-2021.10.1'],
                        'subdir': 'arm',
                    },
                ],
            ),
        ]
    )
    def test_deduplicate_packages(
        self,
        _name,
        packages,
        expected_packages,
    ) -> None:
        """Test package deduplication logic."""
        pkgs = sorted(
            deduplicate_packages(packages),
            key=operator.itemgetter('path'),
        )
        expected_pkgs = sorted(
            expected_packages,
            key=operator.itemgetter('path'),
        )
        self.assertSequenceEqual(expected_pkgs, pkgs)

    def test_all_package_files(self) -> None:
        """Test that CIPD files are loaded in the correct order."""

        upstream_load_order = [
            Path('upstream.json'),
            Path('bazelisk.json'),
            Path('buildifier.json'),
            Path('openjdk.json'),
            Path('cmake.json'),
            Path('coverage.json'),
            Path('default.json'),
            Path('arm.json'),
            Path('pigweed.json'),
            Path('rust.json'),
            Path('clang.json'),
            Path('python.json'),
            Path('python311.json'),
            Path('doxygen.json'),
            Path('flatc.json'),
            Path('go.json'),
            Path('host_tools.json'),
            Path('kythe.json'),
            Path('luci.json'),
            Path('msrv_python.json'),
            Path('rbe.json'),
            Path('testing.json'),
            Path('web.json'),
        ]

        with importlib.resources.as_file(
            importlib.resources.files('pw_env_setup.cipd_setup').joinpath(
                'upstream.json'
            )
        ) as upstream_json:
            all_files = all_package_files(None, [upstream_json])
            all_files_relative = [
                Path(f).relative_to(upstream_json.parent) for f in all_files
            ]
            self.assertEqual(upstream_load_order, all_files_relative)


if __name__ == '__main__':
    unittest.main()
