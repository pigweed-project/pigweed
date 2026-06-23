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
"""Tests for cipd_setup_scanner."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.cipd_setup_scanner import CipdSetupScanner
from pw_fortifier.package_scanner import PackageVersion


class TestCipdSetupScanner(unittest.TestCase):
    """Tests for CipdSetupScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()
        self.cipd_setup_dir = os.path.join(
            self.test_dir, 'pw_env_setup', 'py', 'pw_env_setup', 'cipd_setup'
        )
        os.makedirs(self.cipd_setup_dir)

        # Create root pigweed.json
        self.root_pigweed_json = os.path.join(self.test_dir, 'pigweed.json')
        root_pigweed_content = {
            'pw': {
                'pw_env_setup': {
                    'cipd_package_files': [
                        'pw_env_setup/py/pw_env_setup/cipd_setup/upstream.json'
                    ]
                }
            }
        }
        with open(self.root_pigweed_json, 'w') as f:
            json.dump(root_pigweed_content, f)

        # Write inclusions
        # default.json -> pigweed.json, other.json
        # upstream.json -> default.json, dev_tools.json
        # pigweed.json -> rust.json
        default_json = os.path.join(self.cipd_setup_dir, 'default.json')
        with open(default_json, 'w') as f:
            json.dump({'included_files': ['pigweed.json', 'other.json']}, f)

        upstream_json = os.path.join(self.cipd_setup_dir, 'upstream.json')
        with open(upstream_json, 'w') as f:
            json.dump({'included_files': ['default.json', 'dev_tools.json']}, f)

        pigweed_json = os.path.join(self.cipd_setup_dir, 'pigweed.json')
        pigweed_json_content = {
            'included_files': ['rust.json'],
            'packages': [
                {
                    'path': 'fuchsia/third_party/ninja/${platform}',
                    'platforms': ['linux-amd64', 'mac-amd64'],
                    'tags': ['git_revision:ninja123'],
                },
                {
                    'path': 'fuchsia/third_party/bloaty/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:bloaty123'],
                },
            ],
        }
        with open(pigweed_json, 'w') as f:
            json.dump(pigweed_json_content, f)

        rust_json = os.path.join(self.cipd_setup_dir, 'rust.json')
        rust_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/rust/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:rust123'],
                }
            ]
        }
        with open(rust_json, 'w') as f:
            json.dump(rust_json_content, f)

        dev_tools_json = os.path.join(self.cipd_setup_dir, 'dev_tools.json')
        dev_tools_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/dev_tool/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:dev_tool123'],
                }
            ]
        }
        with open(dev_tools_json, 'w') as f:
            json.dump(dev_tools_json_content, f)

        ignored_json = os.path.join(self.cipd_setup_dir, 'ignored.json')
        ignored_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/ignored/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:ignored123'],
                }
            ]
        }
        with open(ignored_json, 'w') as f:
            json.dump(ignored_json_content, f)

        # Write a fake OWNERS file
        owners_path = os.path.join(self.test_dir, 'OWNERS')
        with open(owners_path, 'w') as f:
            f.write(
                'owner-ninja@google.com\n'
                'owner-bloaty@google.com\n'
                'owner-other@google.com\n'
            )

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
        if cmd[0] == 'git':
            if cmd[1] == 'log':
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout='hash1 Fake commit\n',
                    stderr='',
                )
            if cmd[1] == 'show':
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout='json content\n', stderr=''
                )
            if cmd[1] == 'blame':
                filepath = cmd[-1]
                if os.path.isabs(filepath):
                    rel_path = os.path.relpath(filepath, self.test_dir)
                else:
                    rel_path = filepath

                if 'pigweed.json' in rel_path:
                    stdout = (
                        'hash1 (<owner-ninja@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                elif 'rust.json' in rel_path:
                    stdout = (
                        'hash1 (<owner-bloaty@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                else:
                    stdout = (
                        'hash1 (<owner-other@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=''
                )

        raise ValueError(f'Unexpected command: {cmd}')

    @staticmethod
    def _make_describe_output(pkg: str, date_str: str, tag: str) -> str:
        """Helper to format cipd describe output."""
        parts = tag.split(':')
        val = parts[1] if len(parts) > 1 else tag
        return (
            f'Package: {pkg}\n'
            f'Registered at: {date_str} 12:00:00 -0700 MST\n'
            'Tags:\n'
            f'  git_revision:{val}\n'
        )

    def _mock_run_cipd(self, args):
        """Mock cipd commands."""
        if args[0] == 'ls':
            pass
        elif args[0] == 'instances':
            pkg = args[1]
            parts = pkg.split('/')
            name = parts[-2] if len(parts) >= 2 else 'pkg'
            stdout = (
                "Instance ID                                   "
                "Registered by        Registered at\n"
                "----------------------------------------"
                "-----------------------------------------\n"
                f"git_revision:{name}_latest_hash                "
                "user@google.com      2026-06-08 12:00:00 -0700 MST\n"
                f"git_revision:{name}_mid_hash                   "
                "user@google.com      2026-06-06 12:00:00 -0700 MST\n"
                f"git_revision:{name}123                        "
                "user@google.com      2026-06-05 12:00:00 -0700 MST\n"
            )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )
        if args[0] == 'describe':
            pkg = args[1]
            tag = args[3]
            if '_latest_hash' in tag:
                stdout = self._make_describe_output(pkg, '2026-06-08', tag)
            elif '_mid_hash' in tag:
                stdout = self._make_describe_output(pkg, '2026-06-06', tag)
            elif tag == 'git_revision:ninja123':
                if pkg in (
                    'fuchsia/third_party/ninja/linux-amd64',
                    'fuchsia/third_party/ninja/mac-amd64',
                ):
                    stdout = self._make_describe_output(pkg, '2026-06-05', tag)
            elif tag == 'git_revision:bloaty123':
                if pkg == 'fuchsia/third_party/bloaty/linux-amd64':
                    stdout = self._make_describe_output(pkg, '2026-06-05', tag)
            elif tag == 'git_revision:rust123':
                if pkg == 'fuchsia/third_party/rust/linux-amd64':
                    stdout = self._make_describe_output(pkg, '2026-06-05', tag)
            elif tag == 'git_revision:dev_tool123':
                if pkg == 'fuchsia/third_party/dev_tool/linux-amd64':
                    stdout = self._make_describe_output(pkg, '2026-06-05', tag)
            else:
                raise ValueError(f'Unexpected tag: {tag}')

            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )

        raise ValueError(f'Unexpected cipd args: {args}')

    @patch('subprocess.run')
    def test_scan(self, mock_run):
        """Test scanning starting from pigweed.json."""
        mock_run.side_effect = self._mock_subprocess_run

        scanner = CipdSetupScanner(self.test_dir)
        scanner._run_cipd = self._mock_run_cipd
        scanner.date = date(2026, 6, 10)

        results = list(scanner.pre_scan())

        # We expect 5 results:
        # - ninja (linux-amd64) -> TIER1 (from pigweed.json)
        # - ninja (mac-amd64) -> TIER1 (from pigweed.json)
        # - bloaty (linux-amd64) -> TIER2 (from pigweed.json)
        # - rust (linux-amd64) -> TIER2 (from rust.json)
        # - dev_tool (linux-amd64) -> TIER3 (from dev_tools.json)
        self.assertEqual(len(results), 5)

        # 1. Ninja linux-amd64
        ninja_linux = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/ninja/linux-amd64'
        )
        self.assertEqual(ninja_linux.tier, 1)
        self.assertEqual(ninja_linux.owner, 'owner-ninja@google.com')
        self.assertEqual(
            ninja_linux.current,
            PackageVersion('git_revision:ninja123', date(2026, 6, 5)),
        )
        self.assertEqual(ninja_linux.pkg_type, 'cipd_setup')
        self.assertEqual(
            ninja_linux.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json'
            ),
        )

        # 2. Ninja mac-amd64
        ninja_mac = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/ninja/mac-amd64'
        )
        self.assertEqual(ninja_mac.tier, 1)
        self.assertEqual(ninja_mac.owner, 'owner-ninja@google.com')
        self.assertEqual(
            ninja_mac.current,
            PackageVersion('git_revision:ninja123', date(2026, 6, 5)),
        )
        self.assertEqual(ninja_mac.pkg_type, 'cipd_setup')
        self.assertEqual(
            ninja_mac.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json'
            ),
        )

        # 3. Bloaty linux-amd64
        bloaty_linux = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/bloaty/linux-amd64'
        )
        self.assertEqual(bloaty_linux.tier, 2)
        self.assertEqual(bloaty_linux.owner, 'owner-ninja@google.com')
        self.assertEqual(
            bloaty_linux.current,
            PackageVersion('git_revision:bloaty123', date(2026, 6, 5)),
        )
        self.assertEqual(bloaty_linux.pkg_type, 'cipd_setup')
        self.assertEqual(
            bloaty_linux.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json'
            ),
        )

        # 4. Rust linux-amd64
        rust_linux = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/rust/linux-amd64'
        )
        self.assertEqual(rust_linux.tier, 2)
        self.assertEqual(rust_linux.owner, 'owner-bloaty@google.com')
        self.assertEqual(
            rust_linux.current,
            PackageVersion('git_revision:rust123', date(2026, 6, 5)),
        )
        self.assertEqual(rust_linux.pkg_type, 'cipd_setup')
        self.assertEqual(
            rust_linux.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/rust.json'
            ),
        )

        # 5. Dev tool linux-amd64
        dev_tool = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/dev_tool/linux-amd64'
        )
        self.assertEqual(dev_tool.tier, 3)
        self.assertEqual(dev_tool.owner, 'owner-other@google.com')
        self.assertEqual(
            dev_tool.current,
            PackageVersion('git_revision:dev_tool123', date(2026, 6, 5)),
        )
        self.assertEqual(dev_tool.pkg_type, 'cipd_setup')
        self.assertEqual(
            dev_tool.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/dev_tools.json'
            ),
        )


if __name__ == '__main__':
    unittest.main()
