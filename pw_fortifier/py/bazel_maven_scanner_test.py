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
"""Tests for bazel_maven_scanner."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from pw_fortifier.bazel_maven_scanner import BazelMavenScanner
from pw_fortifier.package_scanner import PackageVersion


class TestBazelMavenScanner(unittest.TestCase):
    """Tests for BazelMavenScanner."""

    def setUp(self):
        """Set up test environment."""
        self.scanner = None
        self.test_dir = tempfile.mkdtemp()
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')

        # Line 2: guava artifact definition
        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'maven.install(\n'
            '    artifacts = [\n'
            '        "com.google.guava:guava:31.1-jre",\n'
            '    ],\n'
            ')\n'
        )
        with open(self.module_bazel, 'w') as f:
            f.write(self.module_bazel_content)

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        with open(self.owners_path, 'w') as f:
            f.write('owner-guava@google.com\n')

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
                    args=cmd, returncode=0, stdout='MODULE.bazel\n', stderr=''
                )
            if cmd[1] == 'blame':
                has_l_arg = '-L' in cmd
                if has_l_arg:
                    l_val = cmd[cmd.index('-L') + 1]
                    filepath = cmd[-1]
                    if os.path.isabs(filepath):
                        rel_path = os.path.relpath(filepath, self.test_dir)
                    else:
                        rel_path = filepath

                    if rel_path == 'MODULE.bazel':
                        if l_val == '4,4':
                            stdout = (
                                'hash1 (<owner-guava@google.com> '
                                '2026-06-01 10:00:00 +0000 4)         '
                                '"com.google.guava:guava:31.1-jre",\n'
                            )
                        else:
                            stdout = ''
                    else:
                        stdout = ''
                else:
                    stdout = ''
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=''
                )

        raise ValueError(f'Unexpected command: {cmd}')

    def _mock_load_repos(self, _module_bazel_path):
        """Mock loading repositories from MODULE.bazel."""
        # Mock return list of (canonical_name, attributes)
        # The maven repo rule will have repositories and artifacts
        repositories = [
            json.dumps({'repo_url': 'https://my.custom.repo/maven'}),
            json.dumps(
                {'repo_url': 'https://repo1.maven.org/maven2'}
            ),  # preferred, should move to front
        ]
        artifacts = [
            json.dumps(
                {
                    'group': 'com.google.guava',
                    'artifact': 'guava',
                    'version': '31.1-jre',
                }
            )
        ]
        attributes = [
            {
                'name': 'repositories',
                'type': 'STRING_LIST',
                'stringListValue': repositories,
            },
            {
                'name': 'artifacts',
                'type': 'STRING_LIST',
                'stringListValue': artifacts,
            },
        ]
        # We mock _repos to return type and owner
        # Owner will be resolved by _find_owners, so repository level owner
        # doesn't matter much.
        self.scanner._repos['+maven+maven'] = ('maven', 'repo-owner@google.com')
        yield '+maven+maven', attributes

    @patch('requests.head')
    @patch('requests.get')
    @patch('subprocess.run')
    def test_scan(self, mock_run, mock_get, mock_head):
        """Test scanning maven dependencies in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run

        # Mock XML metadata response
        xml_content = (
            '<metadata>'
            '  <groupId>com.google.guava</groupId>'
            '  <artifactId>guava</artifactId>'
            '  <versioning>'
            '    <latest>32.0.0-jre</latest>'
            '    <versions>'
            '      <version>31.0-jre</version>'
            '      <version>31.1-jre</version>'
            '      <version>31.2-jre</version>'
            '      <version>32.0.0-jre</version>'
            '    </versions>'
            '  </versioning>'
            '</metadata>'
        )
        mock_get_resp = MagicMock()
        mock_get_resp.status_code = 200
        mock_get_resp.content = xml_content.encode('utf-8')
        mock_get.return_value = mock_get_resp

        # Mock HEAD requests for POM timestamps
        # 31.1-jre -> 2026-06-01
        # 31.2-jre -> 2026-06-02
        # 32.0.0-jre -> 2026-06-03
        def mock_head_side_effect(url, **_kwargs):
            resp = MagicMock()
            resp.status_code = 200
            if '31.1-jre' in url:
                resp.headers = {
                    'Last-Modified': 'Mon, 01 Jun 2026 12:00:00 GMT'
                }
            elif '31.2-jre' in url:
                resp.headers = {
                    'Last-Modified': 'Tue, 02 Jun 2026 12:00:00 GMT'
                }
            elif '32.0.0-jre' in url:
                resp.headers = {
                    'Last-Modified': 'Wed, 03 Jun 2026 12:00:00 GMT'
                }
            else:
                resp.headers = {}
            return resp

        mock_head.side_effect = mock_head_side_effect

        self.scanner = BazelMavenScanner(self.test_dir)
        self.scanner._load_repos = self._mock_load_repos
        self.scanner.date = date(2026, 6, 10)

        results = list(self.scanner.pre_scan())

        self.assertEqual(len(results), 1)

        res = results[0]
        self.assertEqual(res.package, 'com.google.guava:guava')
        self.assertEqual(res.source, 'MODULE.bazel')
        self.assertEqual(res.pkg_type, 'bazel_maven')

        self.assertEqual(
            res.current, PackageVersion('31.1-jre', date(2026, 6, 1))
        )
        self.assertEqual(
            res.earliest, PackageVersion('31.1-jre', date(2026, 6, 1))
        )
        self.assertEqual(res.tier, 2)
        self.assertEqual(res.owner, 'owner-guava@google.com')

        # Verify requests are made to Maven Central first (since it was
        # moved to front).
        mock_get.assert_any_call(
            'https://repo1.maven.org/maven2/com/google/guava/'
            'guava/maven-metadata.xml',
            timeout=10,
        )
        # Should not make requests to custom repo because Central succeeded.
        for call_args in mock_get.call_args_list:
            self.assertNotIn('my.custom.repo', call_args[0][0])


if __name__ == '__main__':
    unittest.main()
