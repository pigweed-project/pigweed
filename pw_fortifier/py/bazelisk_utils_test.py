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
"""Tests for bazelisk_utils in pw_fortifier."""

# pylint: disable=protected-access

import json
import subprocess
import unittest
from unittest.mock import AsyncMock, patch

from pw_fortifier.bazelisk_utils import BazelRepo, run_bazelisk
from pw_fortifier.code_snippet import CodeSnippet


class TestBazeliskUtils(unittest.IsolatedAsyncioTestCase):
    """Tests for BazelRepo and bazelisk utilities."""

    def test_get_attr_str(self) -> None:
        """Tests get_attr_str extracts stringValue."""
        repo = BazelRepo(
            canonical_name='@foo',
            rule_name='cipd_repository',
            location=None,
            attributes=[
                {'name': 'tag', 'stringValue': 'v1.0.0'},
                {'name': 'other', 'stringValue': 'bar'},
            ],
        )
        self.assertEqual(repo.get_attr_str('tag'), 'v1.0.0')
        self.assertEqual(repo.get_attr_str('nonexistent'), '')

    def test_get_attr_str_list(self) -> None:
        """Tests get_attr_str_list extracts stringListValue."""
        repo = BazelRepo(
            canonical_name='@foo',
            rule_name='maven_install',
            location=None,
            attributes=[
                {'name': 'artifacts', 'stringListValue': ['art1', 'art2']},
            ],
        )
        self.assertEqual(repo.get_attr_str_list('artifacts'), ['art1', 'art2'])
        self.assertEqual(repo.get_attr_str_list('nonexistent'), [])

    def test_get_attr_str_dict(self) -> None:
        """Tests get_attr_str_dict extracts stringDictValue."""
        repo = BazelRepo(
            canonical_name='@foo',
            rule_name='package_repo',
            location=None,
            attributes=[
                {
                    'name': 'packages',
                    'stringDictValue': [
                        {'key': 'path/a', 'value': 'tag_a'},
                        {'key': 'path/b', 'value': 'tag_b'},
                    ],
                },
            ],
        )
        self.assertEqual(
            repo.get_attr_str_dict('packages'),
            {'path/a': 'tag_a', 'path/b': 'tag_b'},
        )
        self.assertEqual(repo.get_attr_str_dict('nonexistent'), {})

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch(
        'pw_fortifier.bazelisk_utils.find_location',
        return_value=CodeSnippet('MODULE.bazel', (1, 1)),
    )
    async def test_load_repos(
        self,
        _mock_find_location: AsyncMock,
        mock_run_bazelisk: AsyncMock,
    ) -> None:
        """Tests BazelRepo._load parses dump_repo_mapping and show_repo."""
        mock_mapping = json.dumps({'': 'root', 'my_dep': 'my_dep_canon'})
        mock_show_repo = (
            json.dumps(
                {
                    'canonicalName': 'my_dep_canon',
                    'originalName': 'my_dep',
                    'repoRuleName': 'cipd_repository',
                    'attribute': [{'name': 'tag', 'stringValue': '1.0'}],
                }
            )
            + '\n'
        )

        mock_run_bazelisk.side_effect = [
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_mapping, stderr=''
            ),
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_show_repo, stderr=''
            ),
        ]

        repos = [repo async for repo in BazelRepo.load('/path/to/MODULE.bazel')]
        self.assertEqual(len(repos), 1)
        self.assertEqual(repos[0].canonical_name, 'my_dep_canon')
        self.assertEqual(repos[0].rule_name, 'cipd_repository')
        self.assertEqual(repos[0].location, CodeSnippet('MODULE.bazel', (1, 1)))
        self.assertEqual(repos[0].get_attr_str('tag'), '1.0')

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch('pw_fortifier.bazelisk_utils.find_location')
    async def test_load_repos_canonical_name_formats(
        self,
        mock_find_location: AsyncMock,
        mock_run_bazelisk: AsyncMock,
    ) -> None:
        """Tests parsing canonical names without originalName."""
        mock_mapping = json.dumps(
            {
                '': 'root',
                'aspect_bazel_lib': 'aspect_bazel_lib+',
                'bazel_clang_tidy': '+_repo_rules2+bazel_clang_tidy',
                'pythons_hub': 'rules_python++python+pythons_hub',
            }
        )
        mock_show_repo = (
            '\n'.join(
                [
                    json.dumps(
                        {
                            'canonicalName': 'aspect_bazel_lib+',
                            'repoRuleName': 'bazel_dep',
                        }
                    ),
                    json.dumps(
                        {
                            'canonicalName': ('+_repo_rules2+bazel_clang_tidy'),
                            'repoRuleName': 'git_repository',
                        }
                    ),
                    json.dumps(
                        {
                            'canonicalName': (
                                'rules_python++python+pythons_hub'
                            ),
                            'repoRuleName': 'py_hub',
                        }
                    ),
                ]
            )
            + '\n'
        )

        mock_run_bazelisk.side_effect = [
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_mapping, stderr=''
            ),
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_show_repo, stderr=''
            ),
        ]
        mock_find_location.side_effect = [
            CodeSnippet('MODULE.bazel', (21, 21)),
            CodeSnippet('MODULE.bazel', (132, 132)),
            CodeSnippet('MODULE.bazel', (336, 336)),
        ]

        repos = [repo async for repo in BazelRepo.load('/path/to/MODULE.bazel')]
        self.assertEqual(len(repos), 3)
        self.assertEqual(repos[0].canonical_name, 'aspect_bazel_lib+')
        self.assertEqual(repos[0].rule_name, 'bazel_dep')
        self.assertEqual(
            repos[0].location, CodeSnippet('MODULE.bazel', (21, 21))
        )
        self.assertEqual(
            repos[1].canonical_name, '+_repo_rules2+bazel_clang_tidy'
        )
        self.assertEqual(
            repos[2].canonical_name, 'rules_python++python+pythons_hub'
        )
        self.assertEqual(
            mock_find_location.call_args_list,
            [
                unittest.mock.call(
                    '/path/to', '/path/to/MODULE.bazel', 'aspect_bazel_lib'
                ),
                unittest.mock.call(
                    '/path/to', '/path/to/MODULE.bazel', 'bazel_clang_tidy'
                ),
                unittest.mock.call(
                    '/path/to', '/path/to/MODULE.bazel', 'pythons_hub'
                ),
            ],
        )

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch('pw_fortifier.bazelisk_utils.find_location')
    async def test_load_repos_skips_when_not_in_module_bazel(
        self,
        mock_find_location: AsyncMock,
        mock_run_bazelisk: AsyncMock,
    ) -> None:
        """Tests that repos with no lines in location are skipped."""
        mock_mapping = json.dumps({'': 'root', 'foo': 'foo_canon'})
        mock_show_repo = (
            json.dumps(
                {
                    'canonicalName': 'foo_canon',
                    'repoRuleName': 'git_repository',
                }
            )
            + '\n'
        )
        mock_run_bazelisk.side_effect = [
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_mapping, stderr=''
            ),
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_show_repo, stderr=''
            ),
        ]
        mock_find_location.return_value = CodeSnippet(
            'MODULE.bazel', lines=None
        )

        repos = [repo async for repo in BazelRepo.load('/path/to/MODULE.bazel')]
        self.assertEqual(len(repos), 0)

    @patch('subprocess.run')
    async def test_run_bazelisk(self, mock_subproc_run) -> None:
        """Tests run_bazelisk invokes subprocess."""
        mock_subproc_run.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'test', '//...'],
            returncode=0,
            stdout='ok\n',
            stderr='',
        )
        res = await run_bazelisk(['test', '//...'], cwd='/foo')
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout, 'ok\n')
        mock_subproc_run.assert_called_once_with(
            ['bazelisk', 'test', '//...'],
            cwd='/foo',
            capture_output=True,
            text=True,
        )


if __name__ == '__main__':
    unittest.main()
