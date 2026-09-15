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
"""Tests for bazel_maven."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch, MagicMock

import requests
from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.bazelisk_utils import BazelRepo
from pw_fortifier.bazel_maven import BazelMavenAnalyzer
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestBazelMavenAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for BazelMavenAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.analyzer = None
        self.test_dir = '/test'
        self.working_dir = '/working'
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')
        self._date_patcher = None

        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'maven.install(\n'
            '    artifacts = [\n'
            '        "com.google.guava:guava:31.1-jre",\n'
            '    ],\n'
            ')\n'
        )
        self.fs.create_file(
            self.module_bazel, contents=self.module_bazel_content
        )

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        self.fs.create_file(
            self.owners_path, contents='assignee-guava@google.com\n'
        )

    def tearDown(self):
        """Tear down test environment."""
        if self._date_patcher is not None:
            self._date_patcher.stop()

    def set_date(self, d):
        """Sets the mocked package analyzer date."""
        if self._date_patcher is not None:
            self._date_patcher.stop()
        self._date_patcher = patch('pw_fortifier.package_analyzer.DATE', d)
        self._date_patcher.start()

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
        if cmd[0] == 'git':
            if len(cmd) > 2 and cmd[1] == '-C':
                cmd = [cmd[0]] + cmd[3:]
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
                                'hash1 (<assignee-guava@google.com> '
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

    @staticmethod
    async def _mock_load_repos(_module_bazel_path):
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
        yield BazelRepo(
            canonical_name='+maven+maven',
            rule_name='maven',
            location=CodeSnippet('MODULE.bazel', (3, 3)),
            attributes=attributes,
        )

    @patch('pw_fortifier.bazel_maven.BazelRepo.load')
    @patch('requests.head')
    @patch('requests.get')
    @patch('subprocess.run')
    async def test_run(self, mock_run, mock_get, mock_head, mock_load):
        """Test scanning maven dependencies in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_load.side_effect = self._mock_load_repos

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
        # 31.0-jre -> 2026-05-31
        # 31.1-jre -> 2026-06-01
        # 31.2-jre -> 2026-06-02
        # 32.0.0-jre -> 2026-06-03
        def mock_head_side_effect(url, **_kwargs):
            resp = MagicMock()
            resp.status_code = 200
            if '31.0-jre' in url:
                resp.headers = {
                    'Last-Modified': 'Sun, 31 May 2026 12:00:00 GMT'
                }
            elif '31.1-jre' in url:
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

        self.analyzer = BazelMavenAnalyzer()
        self.set_date(date(2026, 6, 10))

        consumer = PipelineSink()
        await configure_stage_for_test(
            self.analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
        )
        self.analyzer.connect(consumer)
        await self.analyzer.run()

        # Collect results
        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        self.assertEqual(len(results), 1)

        res = results[0]
        self.assertEqual(res.package, 'com.google.guava:guava')
        self.assertEqual(res.source, 'MODULE.bazel')
        self.assertEqual(res.location.file, 'MODULE.bazel')
        self.assertEqual(res.location.lines, (4, 4))
        self.assertEqual(res.pkg_type, 'bazel_maven')

        self.assertEqual(
            res.current, PackageVersion('31.1-jre', date(2026, 6, 1))
        )
        self.assertEqual(
            res.earliest, PackageVersion('31.1-jre', date(2026, 6, 1))
        )
        self.assertEqual(res.tier, 2)
        self.assertIsNone(res.assignee)

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

    @patch('requests.head')
    async def test_get_timestamp_success(self, mock_head: MagicMock) -> None:
        """Tests _get_timestamp parses Last-Modified header."""
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.headers = {'Last-Modified': 'Mon, 01 Jun 2026 12:00:00 GMT'}
        mock_head.return_value = mock_resp

        analyzer = BazelMavenAnalyzer()
        ts = await analyzer._get_timestamp(
            'https://repo1.maven.org/maven2', 'guava', '31.1-jre'
        )
        self.assertEqual(ts, date(2026, 6, 1))

    @patch('requests.head')
    async def test_get_timestamp_missing_last_modified_raises(
        self, mock_head: MagicMock
    ) -> None:
        """Tests _get_timestamp raises AssertionError if header missing."""
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.headers = {}
        mock_head.return_value = mock_resp

        analyzer = BazelMavenAnalyzer()
        with self.assertRaises(AssertionError):
            await analyzer._get_timestamp(
                'https://repo1.maven.org/maven2', 'guava', '31.1-jre'
            )

    @patch('requests.head')
    async def test_get_timestamp_http_error(self, mock_head: MagicMock) -> None:
        """Tests _get_timestamp raises HTTPError when status is not 200."""
        mock_resp = MagicMock()
        mock_resp.status_code = 404
        mock_resp.raise_for_status.side_effect = requests.exceptions.HTTPError(
            '404 Client Error', response=mock_resp
        )
        mock_head.return_value = mock_resp

        analyzer = BazelMavenAnalyzer()
        with self.assertRaises(requests.exceptions.HTTPError):
            await analyzer._get_timestamp(
                'https://repo1.maven.org/maven2', 'guava', '31.1-jre'
            )

    @patch('requests.get')
    async def test_resolve_artifact_http_error(
        self, mock_get: MagicMock
    ) -> None:
        """Tests _resolve_artifact raises HTTPError when status is not 200."""
        mock_resp = MagicMock()
        mock_resp.status_code = 500
        mock_resp.raise_for_status.side_effect = requests.exceptions.HTTPError(
            '500 Server Error', response=mock_resp
        )
        mock_get.return_value = mock_resp

        analyzer = BazelMavenAnalyzer()
        with self.assertRaises(requests.exceptions.HTTPError):
            await analyzer._resolve_artifact(
                'com.google.guava',
                'guava',
                '31.1-jre',
                ['https://repo1.maven.org/maven2'],
            )

    @patch('pw_fortifier.bazel_maven.BazelRepo.load')
    async def test_set_up_unparseable_repo_urls_raises(
        self, mock_load: MagicMock
    ) -> None:
        """Tests _set_up raises ValueError when repo URLs cannot be parsed."""

        async def fake_load(_path):
            yield BazelRepo(
                canonical_name='+maven+maven',
                rule_name='maven',
                location=CodeSnippet('MODULE.bazel', (3, 3)),
                attributes=[
                    {
                        'name': 'repositories',
                        'type': 'STRING_LIST',
                        'stringListValue': ['{"invalid": "data"}'],
                    },
                    {
                        'name': 'artifacts',
                        'type': 'STRING_LIST',
                        'stringListValue': ['{"artifact": "foo"}'],
                    },
                ],
            )

        mock_load.side_effect = fake_load

        analyzer = BazelMavenAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
        )

        with self.assertRaises(ValueError):
            await analyzer._set_up()

    async def test_configure_skip_setup(self) -> None:
        """Tests configure sets skip_setup when MODULE.bazel is not in files."""
        analyzer = BazelMavenAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=None,
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelMavenAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['MODULE.bazel'],
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelMavenAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['other.txt'],
        )
        self.assertTrue(analyzer.skip_setup)


if __name__ == '__main__':
    unittest.main()
