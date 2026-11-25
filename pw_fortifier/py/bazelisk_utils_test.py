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

import contextlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import AsyncMock, patch

from pw_fortifier.bazelisk_utils import (
    BazelRepo,
    find_bazel_dep_location,
    find_module_location,
    is_module_file_selected,
    module_bazel_files,
    run_bazelisk,
)
from pw_fortifier.code_snippet import CodeSnippet


def _write_split_module(root: Path) -> None:
    """Writes a MODULE.bazel split across single- and multi-line include()s."""
    (root / 'bazel').mkdir()
    (root / 'MODULE.bazel').write_text(
        '"""\ninclude("//bazel:in_docstring.MODULE.bazel")\n"""\n'
        'module(name = "example")\n'
        'bazel_dep(name = "core_dep", version = "1.0")\n'
        '# include("//bazel:commented_out.MODULE.bazel")\n'
        'include("//bazel:resolve.MODULE.bazel")\n'
        'include(\n'
        '    "//bazel:dev.MODULE.bazel",\n'
        ')\n',
        encoding='utf-8',
    )
    (root / 'bazel' / 'resolve.MODULE.bazel').write_text(
        'single_version_override(module_name = "core_dep")\n',
        encoding='utf-8',
    )
    (root / 'bazel' / 'dev.MODULE.bazel').write_text(
        '# Dev-only declarations.\n'
        'bazel_dep(name = "dev_dep", version = "2.0", dev_dependency = True)\n'
        "include('//:nested.MODULE.bazel')\n",
        encoding='utf-8',
    )
    (root / 'nested.MODULE.bazel').write_text(
        'use_repo(cipd_dev_ext, nested_tool = "example.nested_tool")\n',
        encoding='utf-8',
    )


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
                    Path('/path/to'),
                    Path('/path/to/MODULE.bazel'),
                    'aspect_bazel_lib',
                ),
                unittest.mock.call(
                    Path('/path/to'),
                    Path('/path/to/MODULE.bazel'),
                    'bazel_clang_tidy',
                ),
                unittest.mock.call(
                    Path('/path/to'),
                    Path('/path/to/MODULE.bazel'),
                    'pythons_hub',
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

    def test_module_bazel_files(self) -> None:
        """Tests that included module files are found in include order."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_split_module(root)
            self.assertEqual(
                module_bazel_files(root / 'MODULE.bazel'),
                [
                    root / 'MODULE.bazel',
                    root / 'bazel' / 'resolve.MODULE.bazel',
                    root / 'bazel' / 'dev.MODULE.bazel',
                    root / 'nested.MODULE.bazel',
                ],
            )

    def test_module_bazel_files_without_includes(self) -> None:
        """Tests that only the root file is returned without include()s."""
        with tempfile.TemporaryDirectory() as tmp:
            module_bazel = Path(tmp) / 'MODULE.bazel'
            module_bazel.write_text('module(name = "example")\n')
            self.assertEqual(module_bazel_files(module_bazel), [module_bazel])
        # An unreadable root file is still reported so callers can name it.
        self.assertEqual(
            module_bazel_files('/nonexistent/MODULE.bazel'),
            [Path('/nonexistent/MODULE.bazel')],
        )

    def test_module_bazel_files_ignores_unsafe_includes(self) -> None:
        """Tests that include() cannot name files outside the repository."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'repo'
            root.mkdir()
            outside = Path(tmp) / 'outside.MODULE.bazel'
            outside.write_text('bazel_dep(name = "outside", version = "1")\n')
            (root / 'ok.MODULE.bazel').write_text('')
            (root / 'notes.txt').write_text('')
            # These exist, but only labels that Bazel rejects name them.
            (root / 'sub').mkdir()
            (root / 'sub' / 'backslash.MODULE.bazel').write_text('')
            (root / 'dot.MODULE.bazel').write_text('')
            (root / 'up.MODULE.bazel').write_text('')
            (root / 'MODULE.bazel').write_text(
                # Use "/" so the line is a valid string literal on Windows.
                f'include("//:{outside.as_posix()}")\n'
                'include("//:sub\\\\backslash.MODULE.bazel")\n'
                'include("//:./dot.MODULE.bazel")\n'
                'include("//sub/..:up.MODULE.bazel")\n'
                'include("//:C:/up.MODULE.bazel")\n'
                'include("//up.MODULE.bazel")\n'
                'include("//:/etc/passwd")\n'
                'include("//:/outside.MODULE.bazel")\n'
                'include("//:../outside.MODULE.bazel")\n'
                'include("//..:outside.MODULE.bazel")\n'
                'include("@other//bazel:ok.MODULE.bazel")\n'
                'include("//:missing.MODULE.bazel")\n'
                'include("//:notes.txt")\n'
                'include("//:ok.MODULE.bazel")\n'
            )
            self.assertEqual(
                module_bazel_files(root / 'MODULE.bazel'),
                [root / 'MODULE.bazel', root / 'ok.MODULE.bazel'],
            )

    def test_module_bazel_files_warns_on_parse_failure(self) -> None:
        """Tests that a module file that cannot be parsed is reported."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'ok.MODULE.bazel').write_text('')
            module_bazel = root / 'MODULE.bazel'
            module_bazel.write_text(
                'include("//:ok.MODULE.bazel")\nbazel_dep(name = \n'
            )
            with self.assertLogs(
                'pw_fortifier.bazelisk_utils', level='WARNING'
            ) as logs:
                files = module_bazel_files(module_bazel)
            self.assertEqual(files, [module_bazel])
            self.assertEqual(len(logs.output), 1)
            self.assertIn(str(module_bazel), logs.output[0])
            self.assertIn('ignoring its include() calls', logs.output[0])

    def test_module_bazel_files_depth_first_order(self) -> None:
        """Tests that nested includes come before later sibling includes."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bazel').mkdir()
            (root / 'MODULE.bazel').write_text(
                'include("//bazel:resolve.MODULE.bazel")\n'
                'include("//bazel:dev.MODULE.bazel")\n',
                encoding='utf-8',
            )
            (root / 'bazel' / 'resolve.MODULE.bazel').write_text(
                'include("//:nested.MODULE.bazel")\n',
                encoding='utf-8',
            )
            (root / 'nested.MODULE.bazel').write_text('', encoding='utf-8')
            (root / 'bazel' / 'dev.MODULE.bazel').write_text(
                '', encoding='utf-8'
            )
            self.assertEqual(
                module_bazel_files(root / 'MODULE.bazel'),
                [
                    root / 'MODULE.bazel',
                    root / 'bazel' / 'resolve.MODULE.bazel',
                    root / 'nested.MODULE.bazel',
                    root / 'bazel' / 'dev.MODULE.bazel',
                ],
            )

    def test_module_bazel_files_warns_on_unusable_file(self) -> None:
        """Tests that files Python cannot read or parse do not raise."""
        include = b'include("//:ok.MODULE.bazel")\n'
        cases = (
            # Null bytes: ValueError before Python 3.12.
            (include + b'\0', 'Cannot parse'),
            # Nesting too deep for the parser: MemoryError.
            (include + b'x = ' + b'-' * 20000 + b'1', 'Cannot parse'),
            # Invalid UTF-8.
            (include + b'\xff', 'Cannot read'),
        )
        for contents, problem in cases:
            with self.subTest(contents=contents[-8:]):
                with tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / 'ok.MODULE.bazel').write_text('')
                    module_bazel = root / 'MODULE.bazel'
                    module_bazel.write_bytes(contents)
                    with self.assertLogs(
                        'pw_fortifier.bazelisk_utils', level='WARNING'
                    ) as logs:
                        files = module_bazel_files(module_bazel)
                    self.assertEqual(files, [module_bazel])
                    self.assertEqual(len(logs.output), 1)
                    self.assertIn(f'{problem} {module_bazel}', logs.output[0])
                    self.assertIn(
                        'ignoring its include() calls', logs.output[0]
                    )

    def test_module_bazel_files_survives_deep_nesting(self) -> None:
        """Tests that nesting some Python versions reject does not raise."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ok_module = root / 'ok.MODULE.bazel'
            ok_module.write_text('')
            module_bazel = root / 'MODULE.bazel'
            module_bazel.write_text(
                'include("//:ok.MODULE.bazel")\nx = a' + '.b' * 20000
            )
            # Raises RecursionError inside ast.parse() before Python 3.14.
            self.assertIn(
                module_bazel_files(module_bazel),
                ([module_bazel], [module_bazel, ok_module]),
            )

    def test_module_bazel_files_warns_on_read_error(self) -> None:
        """Tests that an included file that cannot be read is reported."""
        real_read_text = Path.read_text

        def read_text(path: Path, *args, **kwargs) -> str:
            if path.name == 'dev.MODULE.bazel':
                raise PermissionError('denied')
            return real_read_text(path, *args, **kwargs)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_split_module(root)
            dev_module = root / 'bazel' / 'dev.MODULE.bazel'
            with patch.object(Path, 'read_text', read_text):
                with self.assertLogs(
                    'pw_fortifier.bazelisk_utils', level='WARNING'
                ) as logs:
                    files = module_bazel_files(root / 'MODULE.bazel')
            # The file is still listed, but the file it includes is not.
            self.assertEqual(
                files,
                [
                    root / 'MODULE.bazel',
                    root / 'bazel' / 'resolve.MODULE.bazel',
                    dev_module,
                ],
            )
            self.assertEqual(len(logs.output), 1)
            self.assertIn(f'Cannot read {dev_module}', logs.output[0])

    @unittest.skipIf(os.name == 'nt', 'Creating symlinks needs privileges')
    def test_module_bazel_files_visits_symlinked_file_once(self) -> None:
        """Tests that a file and the symlinks to it are listed only once."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'MODULE.bazel').write_text(
                'include("//:alias.MODULE.bazel")\n'
                'include("//:real.MODULE.bazel")\n'
            )
            (root / 'real.MODULE.bazel').write_text(
                'include("//:loop.MODULE.bazel")\n'
            )
            (root / 'alias.MODULE.bazel').symlink_to(root / 'real.MODULE.bazel')
            (root / 'loop.MODULE.bazel').symlink_to(root / 'MODULE.bazel')
            self.assertEqual(
                module_bazel_files(root / 'MODULE.bazel'),
                [root / 'MODULE.bazel', root / 'alias.MODULE.bazel'],
            )
            # The file is selected by the symlink's path or by its target's.
            for selected in (
                'alias.MODULE.bazel',
                'real.MODULE.bazel',
                str(root / 'alias.MODULE.bazel'),
                str(root / 'real.MODULE.bazel'),
            ):
                with self.subTest(selected=selected):
                    self.assertTrue(is_module_file_selected(tmp, [selected]))

    @unittest.skipIf(os.name == 'nt', 'Creating symlinks needs privileges')
    def test_module_bazel_files_ignores_symlinks_out_of_repo(self) -> None:
        """Tests that an included symlink cannot escape the repository."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'repo'
            root.mkdir()
            outside = Path(tmp) / 'outside.MODULE.bazel'
            outside.write_text('bazel_dep(name = "outside", version = "1")\n')
            (root / 'link.MODULE.bazel').symlink_to(outside)
            (root / 'MODULE.bazel').write_text(
                'include("//:link.MODULE.bazel")\n'
            )
            self.assertEqual(
                module_bazel_files(root / 'MODULE.bazel'),
                [root / 'MODULE.bazel'],
            )

    def test_find_module_location(self) -> None:
        """Tests that a declaration is found in whichever file has it."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_split_module(root)
            files = module_bazel_files(root / 'MODULE.bazel')

            self.assertEqual(
                find_module_location(tmp, files, 'bazel_dep', 'core_dep'),
                CodeSnippet('MODULE.bazel', (5, 5)),
            )
            self.assertEqual(
                find_module_location(tmp, files, 'bazel_dep', 'dev_dep'),
                CodeSnippet(os.path.join('bazel', 'dev.MODULE.bazel'), (2, 2)),
            )
            self.assertEqual(
                find_module_location(tmp, files, 'nested_tool'),
                CodeSnippet('nested.MODULE.bazel', (1, 1)),
            )
            # Nothing matches: report the root file with no line.
            self.assertEqual(
                find_module_location(tmp, files, 'missing_dep'),
                CodeSnippet('MODULE.bazel', None),
            )
            with self.assertRaises(ValueError):
                find_module_location(tmp, [], 'core_dep')

    def test_find_module_location_relative_root(self) -> None:
        """Tests that a relative root directory is not joined twice."""
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'repo').mkdir()
            _write_split_module(Path(tmp) / 'repo')
            with contextlib.chdir(tmp):
                root = Path('repo')
                files = module_bazel_files(root / 'MODULE.bazel')
                self.assertEqual(
                    find_module_location(root, files, 'bazel_dep', 'dev_dep'),
                    CodeSnippet(
                        os.path.join('bazel', 'dev.MODULE.bazel'), (2, 2)
                    ),
                )
                self.assertEqual(
                    find_module_location(root, files, 'missing_dep'),
                    CodeSnippet('MODULE.bazel', None),
                )
                self.assertEqual(
                    find_bazel_dep_location(root, files, 'dev_dep'),
                    CodeSnippet(
                        os.path.join('bazel', 'dev.MODULE.bazel'), (2, 2)
                    ),
                )

    def test_find_bazel_dep_location(self) -> None:
        """Tests that bazel_dep() calls are found however they are wrapped."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bazel').mkdir()
            (root / 'MODULE.bazel').write_text(
                '# bazel_dep(name = "wrapped_dep", version = "0.1")\n'
                'bazel_dep(name = "rules_foo_extra", version = "1.0")\n'
                'bazel_dep(name = "rules_foo", version = "1.0")\n'
                'bazel_dep("positional_dep", "1.0")\n'
                'include("//bazel:dev.MODULE.bazel")\n',
                encoding='utf-8',
            )
            (root / 'bazel' / 'dev.MODULE.bazel').write_text(
                '# Dev-only declarations.\n'
                'bazel_dep(\n'
                '    name = "wrapped_dep",\n'
                '    version = "2.0",\n'
                '    dev_dependency = True,\n'
                ')\n',
                encoding='utf-8',
            )
            files = module_bazel_files(root / 'MODULE.bazel')
            dev_module = os.path.join('bazel', 'dev.MODULE.bazel')

            # A declaration wrapped across lines, not the comment naming it.
            self.assertEqual(
                find_bazel_dep_location(tmp, files, 'wrapped_dep'),
                CodeSnippet(dev_module, (2, 6)),
            )
            # The exact module name, not a longer name that contains it.
            self.assertEqual(
                find_bazel_dep_location(tmp, files, 'rules_foo'),
                CodeSnippet('MODULE.bazel', (3, 3)),
            )
            self.assertEqual(
                find_bazel_dep_location(tmp, files, 'positional_dep'),
                CodeSnippet('MODULE.bazel', (4, 4)),
            )
            self.assertEqual(
                find_bazel_dep_location(tmp, files, 'missing_dep'),
                CodeSnippet('MODULE.bazel', None),
            )

    def test_find_bazel_dep_location_in_unparseable_file(self) -> None:
        """Tests that the text of a file that cannot be parsed is searched."""
        with tempfile.TemporaryDirectory() as tmp:
            module_bazel = Path(tmp) / 'MODULE.bazel'
            module_bazel.write_text(
                'bazel_dep(name = "core_dep", version = "1.0")\n'
                'bazel_dep(name = \n',
                encoding='utf-8',
            )
            self.assertEqual(
                find_bazel_dep_location(tmp, [module_bazel], 'core_dep'),
                CodeSnippet('MODULE.bazel', (1, 1)),
            )

    def test_is_module_file_selected_glob_segments(self) -> None:
        """Tests that wildcards match path segments as Path.glob() does."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bazel').mkdir()
            (root / 'MODULE.bazel').write_text(
                'include("//bazel:dev.MODULE.bazel")\n', encoding='utf-8'
            )
            (root / 'bazel' / 'dev.MODULE.bazel').write_text(
                '', encoding='utf-8'
            )
            for pattern in (
                'bazel/**/*.MODULE.bazel',
                '**/*.MODULE.bazel',
                '**/dev.MODULE.bazel',
                '*/dev.MODULE.bazel',
            ):
                with self.subTest(pattern=pattern):
                    self.assertTrue(is_module_file_selected(tmp, [pattern]))
            # "*" does not cross a directory separator.
            for pattern in ('*.MODULE.bazel', 'bazel/*/dev.MODULE.bazel'):
                with self.subTest(pattern=pattern):
                    self.assertFalse(is_module_file_selected(tmp, [pattern]))

    def test_is_module_file_selected(self) -> None:
        """Tests that included module files, wildcards, and separators match."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_split_module(root)
            self.assertTrue(is_module_file_selected(tmp, ['MODULE.bazel']))
            self.assertTrue(is_module_file_selected(tmp, ['**/MODULE.bazel']))
            self.assertTrue(
                is_module_file_selected(
                    tmp, ['a.txt', 'bazel/dev.MODULE.bazel']
                )
            )
            self.assertTrue(
                is_module_file_selected(tmp, ['bazel\\dev.MODULE.bazel'])
            )
            self.assertTrue(
                is_module_file_selected(tmp, ['bazel/*.MODULE.bazel'])
            )
            self.assertTrue(
                is_module_file_selected(
                    tmp, [str(root / 'bazel' / 'dev.MODULE.bazel')]
                )
            )
            self.assertTrue(
                is_module_file_selected(tmp, ['nested.MODULE.bazel'])
            )
            self.assertFalse(is_module_file_selected(tmp, []))
            self.assertFalse(is_module_file_selected(tmp, ['dev.MODULE.bazel']))
            self.assertFalse(
                is_module_file_selected(
                    tmp, ['bazel/commented_out.MODULE.bazel']
                )
            )

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    async def test_load_repos_from_included_files(
        self, mock_run_bazelisk: AsyncMock
    ) -> None:
        """Tests that repos declared in included files are not skipped."""
        mock_mapping = json.dumps(
            {
                '': 'root',
                'core_dep': 'core_dep+',
                'nested_tool': '+cipd+example.nested_tool',
                'unlisted': 'unlisted+',
            }
        )
        mock_show_repo = ''.join(
            json.dumps({'canonicalName': name, 'repoRuleName': rule}) + '\n'
            for name, rule in (
                ('core_dep+', 'http_archive'),
                ('+cipd+example.nested_tool', 'package_repo'),
                ('unlisted+', 'http_archive'),
            )
        )
        mock_run_bazelisk.side_effect = [
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_mapping, stderr=''
            ),
            subprocess.CompletedProcess(
                args=[], returncode=0, stdout=mock_show_repo, stderr=''
            ),
        ]

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_split_module(root)
            repos = [
                repo async for repo in BazelRepo.load(root / 'MODULE.bazel')
            ]

        self.assertEqual(
            [(repo.canonical_name, repo.location) for repo in repos],
            [
                ('core_dep+', CodeSnippet('MODULE.bazel', (5, 5))),
                (
                    '+cipd+example.nested_tool',
                    CodeSnippet('nested.MODULE.bazel', (1, 1)),
                ),
            ],
        )

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
