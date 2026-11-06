#!/usr/bin/env python3
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
"""Tests for pw_cli.requires.

These tests use real git repositories in temporary directories. A local bare
repository stands in for the requires-helper repository, and a fake './gh'
script serves canned JSON responses and records the arguments it was called
with.
"""

import io
import json
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

from pw_cli import requires
from pw_cli.requires import Change, Requirement

# These tests also cover the module's private helpers.
# pylint: disable=protected-access

_CHANGE_ID_A = 'I' + 'a' * 40
_CHANGE_ID_B = 'I' + 'b' * 40
_CHANGE_ID_C = 'I' + 'c' * 40

_FAKE_GH = '''\
"""Fake './gh' for requires_test.py."""
import json
import sys
from pathlib import Path

state = Path({state!r})
with open(state / 'calls.jsonl', 'a') as outs:
    outs.write(json.dumps(sys.argv[1:]) + '\\n')
responses = json.loads((state / 'responses.json').read_text())
for response in responses:
    if response['args'] == sys.argv[1:]:
        sys.stdout.write(response['stdout'])
        sys.stderr.write(response['stderr'])
        sys.exit(response['returncode'])
sys.stderr.write(f'unexpected arguments: {{sys.argv[1:]}}\\n')
sys.exit(99)
'''

# Output printed by the helper repository's pre-receive hook. Git relays it to
# the pusher prefixed by 'remote:', like Gerrit's output for a new change.
_PUSHED_HELPER_NUMBER = 4242
_PRE_RECEIVE_HOOK = f'''\
#!/bin/sh
echo
echo "  https://{requires.HELPER_GERRIT}-review.googlesource.com/c/\
{requires.HELPER_PROJECT}/+/{_PUSHED_HELPER_NUMBER} Helper change [NEW]"
echo
'''


def _view_args(gerrit_name: str, number: int) -> list[str]:
    return [
        *('--host', f'{gerrit_name}-review.googlesource.com'),
        *('pr', 'view', str(number)),
        *('--json', 'number,commitMessage'),
    ]


def _search_args(gerrit_name: str, change_id: str) -> list[str]:
    return [
        *('--host', f'{gerrit_name}-review.googlesource.com'),
        *('pr', 'list', '--all-projects', '--state', 'all'),
        *('--limit', '100', '--search', f'change:{change_id}'),
        *('--json', 'number,project,branch,status,title'),
    ]


def _search_result(number: int, branch: str = 'main') -> dict[str, Any]:
    return {
        'number': number,
        'project': 'some/project',
        'branch': branch,
        'status': 'NEW',
        'title': f'Change {number}',
    }


class GitTestCase(unittest.TestCase):
    """Provides an isolated git environment and helpers for tests."""

    def setUp(self) -> None:
        super().setUp()
        # Bazel's test sandbox doesn't always have git on PATH.
        if shutil.which('git') is None:
            self.skipTest('git is not available')

        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

        home = self.tmp / 'home'
        home.mkdir()
        env = mock.patch.dict(
            os.environ,
            {
                'HOME': str(home),
                'XDG_CONFIG_HOME': str(home),
                'GIT_CONFIG_NOSYSTEM': '1',
                'GIT_CEILING_DIRECTORIES': str(self.tmp),
                'GIT_AUTHOR_NAME': 'Test',
                'GIT_AUTHOR_EMAIL': 'test@example.com',
                'GIT_COMMITTER_NAME': 'Test',
                'GIT_COMMITTER_EMAIL': 'test@example.com',
            },
        )
        env.start()
        self.addCleanup(env.stop)

        self.addCleanup(os.chdir, os.getcwd())

    @staticmethod
    def git(cwd: Path, *args: str) -> str:
        return subprocess.run(
            ['git', *args],
            cwd=cwd,
            capture_output=True,
            check=True,
            text=True,
        ).stdout

    def init_repo(self, name: str, message: str = 'Initial commit') -> Path:
        path = self.tmp / name
        path.mkdir()
        self.git(path, 'init', '--quiet', '--initial-branch=main')
        (path / 'file.txt').write_text('contents\n')
        self.git(path, 'add', 'file.txt')
        self.git(path, 'commit', '--quiet', '-m', message)
        return path

    def init_helper_repo(self, hook: bool = True) -> Path:
        """Creates a bare repository that stands in for requires-helper."""
        source = self.init_repo('helper-source')
        path = self.tmp / 'helper.git'
        self.git(self.tmp, 'clone', '--quiet', '--bare', str(source), str(path))
        if hook:
            hook_path = path / 'hooks' / 'pre-receive'
            hook_path.write_text(_PRE_RECEIVE_HOOK)
            hook_path.chmod(0o755)
        return path

    def message(self, repo: Path, rev: str = 'HEAD') -> str:
        return self.git(repo, 'log', '-1', '--format=%B', rev)

    def subject(self, repo: Path, rev: str = 'HEAD') -> str:
        return self.git(repo, 'log', '-1', '--format=%s', rev).strip()

    def trailers(self, repo: Path, rev: str = 'HEAD') -> list[tuple[str, str]]:
        parsed = subprocess.run(
            ['git', 'interpret-trailers', '--parse'],
            input=self.message(repo, rev),
            capture_output=True,
            check=True,
            text=True,
        ).stdout
        result = []
        for line in parsed.splitlines():
            key, _, value = line.partition(':')
            result.append((key.strip(), value.strip()))
        return result

    def show_json(self, repo: Path, rev: str, path: str) -> Any:
        return json.loads(self.git(repo, 'show', f'{rev}:{path}'))


class FakeGhMixin(GitTestCase):
    """Installs a fake './gh' script into a repository."""

    def setUp(self) -> None:
        # './gh' is a shell script, which Windows can't run directly. Also,
        # os.access() reports all files as executable on Windows.
        if sys.platform == 'win32':
            self.skipTest("'./gh' isn't supported on Windows")
        super().setUp()

    def install_fake_gh(self, repo: Path, name: str = 'gh') -> Path:
        self.gh_state = self.tmp / 'gh-state'
        self.gh_state.mkdir(exist_ok=True)
        self.set_gh_responses([])

        script = self.gh_state / 'fake_gh.py'
        script.write_text(_FAKE_GH.format(state=str(self.gh_state)))

        gh = repo / name
        gh.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n')
        gh.chmod(0o755)
        return gh

    def set_gh_responses(self, responses: list[dict[str, Any]]) -> None:
        (self.gh_state / 'responses.json').write_text(json.dumps(responses))

    def add_view_response(
        self, gerrit_name: str, number: int, message: str
    ) -> None:
        self._add_response(
            _view_args(gerrit_name, number),
            stdout=json.dumps({'number': number, 'commitMessage': message}),
        )

    def add_search_response(
        self, gerrit_name: str, change_id: str, results: list[dict[str, Any]]
    ) -> None:
        self._add_response(
            _search_args(gerrit_name, change_id), stdout=json.dumps(results)
        )

    def add_failure_response(self, args: list[str], stderr: str) -> None:
        self._add_response(args, stderr=stderr, returncode=1)

    def _add_response(
        self,
        args: list[str],
        stdout: str = '',
        stderr: str = '',
        returncode: int = 0,
    ) -> None:
        path = self.gh_state / 'responses.json'
        responses = json.loads(path.read_text())
        responses.append(
            {
                'args': args,
                'stdout': stdout,
                'stderr': stderr,
                'returncode': returncode,
            }
        )
        path.write_text(json.dumps(responses))

    def gh_calls(self) -> list[list[str]]:
        path = self.gh_state / 'calls.jsonl'
        if not path.is_file():
            return []
        return [json.loads(x) for x in path.read_text().splitlines()]


class RequirementTest(unittest.TestCase):
    """Tests for Requirement and parse_requirement()."""

    def test_str_with_number(self) -> None:
        self.assertEqual(str(Requirement('pigweed', number=123)), 'pigweed:123')

    def test_str_with_change_id(self) -> None:
        self.assertEqual(
            str(Requirement('pigweed', change_id=_CHANGE_ID_A)),
            f'pigweed:{_CHANGE_ID_A}',
        )

    def test_str_prefers_number(self) -> None:
        req = Requirement('pigweed', number=123, change_id=_CHANGE_ID_A)
        self.assertEqual(str(req), 'pigweed:123')

    def test_parse_number(self) -> None:
        self.assertEqual(
            requires.parse_requirement('secret-project:1234'),
            Requirement('secret-project', number=1234),
        )

    def test_parse_change_id(self) -> None:
        self.assertEqual(
            requires.parse_requirement(f'secret-project:{_CHANGE_ID_A}'),
            Requirement('secret-project', change_id=_CHANGE_ID_A),
        )

    def test_parse_invalid(self) -> None:
        for req in (
            '',
            'pigweed',
            'pigweed:',
            ':1234',
            'pigweed:abc',
            'pigweed:-1',
            'pigweed:I1234',
            'pigweed:' + 'a' * 40,
            'pigweed:I' + 'A' * 40,
            'pigweed:I' + 'a' * 41,
        ):
            with self.subTest(req=req):
                with self.assertRaises(ValueError):
                    requires.parse_requirement(req)


class JsonTest(unittest.TestCase):
    """Tests for EnhancedJSONEncoder and dump_json_patches()."""

    def test_encoder_dataclass(self) -> None:
        encoded = json.dumps(
            Change('pigweed', 123), cls=requires.EnhancedJSONEncoder
        )
        self.assertEqual(
            json.loads(encoded), {'gerrit_name': 'pigweed', 'number': 123}
        )

    def test_encoder_unsupported_type(self) -> None:
        with self.assertRaises(TypeError):
            json.dumps(object(), cls=requires.EnhancedJSONEncoder)

    def test_dump_json_patches(self) -> None:
        outs = io.StringIO()
        requires.dump_json_patches(
            [
                {'gerrit_name': 'existing', 'number': 1},  # type: ignore
                Change('pigweed', 123),
            ],
            outs,
        )
        self.assertEqual(
            json.loads(outs.getvalue()),
            [
                {'gerrit_name': 'existing', 'number': 1},
                {'gerrit_name': 'pigweed', 'number': 123},
            ],
        )


class LogEntryExitTest(unittest.TestCase):
    """Tests for log_entry_exit()."""

    def test_returns_value(self) -> None:
        @requires.log_entry_exit
        def add(a: int, b: int) -> int:
            return a + b

        with self.assertLogs(requires._LOG, logging.DEBUG) as logs:
            self.assertEqual(add(1, b=2), 3)
        output = '\n'.join(logs.output)
        self.assertIn('entering add()', output)
        self.assertIn('return value 3', output)
        self.assertIn('exiting add()', output)

    def test_reraises(self) -> None:
        @requires.log_entry_exit
        def fail() -> None:
            raise KeyError('oops')

        with self.assertLogs(requires._LOG, logging.DEBUG) as logs:
            with self.assertRaises(KeyError):
                fail()
        output = '\n'.join(logs.output)
        self.assertIn('exception', output)
        self.assertIn('exiting fail()', output)


class ParseArgsTest(unittest.TestCase):
    """Tests for parse_args()."""

    def test_requirements(self) -> None:
        argv = ['requires', 'a:1', f'b:{_CHANGE_ID_A}']
        with mock.patch.object(sys, 'argv', argv):
            args = requires.parse_args()
        self.assertEqual(args.requirements, ['a:1', f'b:{_CHANGE_ID_A}'])
        self.assertTrue(args.push)

    def test_no_push(self) -> None:
        with mock.patch.object(sys, 'argv', ['requires', '--no-push', 'a:1']):
            self.assertFalse(requires.parse_args().push)

    def test_requires_requirement(self) -> None:
        with mock.patch.object(sys, 'argv', ['requires']):
            with mock.patch.object(sys, 'stderr', io.StringIO()):
                with self.assertRaises(SystemExit):
                    requires.parse_args()


class RunCommandTest(unittest.TestCase):
    """Tests for _run_command()."""

    def test_captures_output(self) -> None:
        res = requires._run_command(
            [sys.executable, '-c', 'import sys; print(sys.stdin.read())'],
            input=b'hello',
        )
        self.assertEqual(res.stdout.strip(), b'hello')

    def test_raises_on_failure(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError) as ctx:
            requires._run_command(
                [
                    sys.executable,
                    '-c',
                    'import sys; sys.stderr.write("bad"); sys.exit(3)',
                ]
            )
        self.assertEqual(ctx.exception.returncode, 3)
        self.assertEqual(ctx.exception.stderr, b'bad')


class GitHelpersTest(GitTestCase):
    """Tests for _git_root(), check_status(), and clone()."""

    def test_git_root(self) -> None:
        repo = self.init_repo('repo')
        subdir = repo / 'sub' / 'dir'
        subdir.mkdir(parents=True)
        os.chdir(subdir)
        self.assertEqual(requires._git_root().resolve(), repo.resolve())

    def test_check_status_in_repo(self) -> None:
        os.chdir(self.init_repo('repo'))
        self.assertTrue(requires.check_status())

    def test_check_status_outside_repo(self) -> None:
        not_repo = self.tmp / 'not-repo'
        not_repo.mkdir()
        os.chdir(not_repo)
        with self.assertLogs(requires._LOG, logging.ERROR):
            self.assertFalse(requires.check_status())

    def test_clone(self) -> None:
        helper = self.init_helper_repo()
        dest = self.tmp / 'dest'
        dest.mkdir()
        with mock.patch.object(requires, 'HELPER_REPO', str(helper)):
            requires.clone(dest)
        self.assertTrue((dest / 'file.txt').is_file())
        self.assertEqual(self.subject(dest), 'Initial commit')


class FindGhTest(FakeGhMixin):
    """Tests for find_gh()."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.init_repo('repo')
        os.chdir(self.repo)

        # Put a directory of fake tools at the front of PATH. Keep the rest of
        # PATH since git may be a wrapper that needs other tools.
        self.bin_dir = self.tmp / 'bin'
        self.bin_dir.mkdir()
        path = os.pathsep.join((str(self.bin_dir), os.environ['PATH']))
        env = mock.patch.dict(os.environ, {'PATH': path})
        env.start()
        self.addCleanup(env.stop)

    def test_gh_in_repo_root(self) -> None:
        gh = self.install_fake_gh(self.repo)
        self.install_fake_gh(self.bin_dir, name='gh-ish')
        self.assertEqual(Path(requires.find_gh()).resolve(), gh.resolve())

    def test_gh_ish_on_path(self) -> None:
        (self.repo / 'gh').write_text('not executable\n')
        gh_ish = self.install_fake_gh(self.bin_dir, name='gh-ish')
        self.assertEqual(Path(requires.find_gh()).resolve(), gh_ish.resolve())

    def test_not_found(self) -> None:
        if shutil.which('gh-ish'):
            self.skipTest('gh-ish is installed on PATH')
        with self.assertRaisesRegex(FileNotFoundError, "couldn't find"):
            requires.find_gh()


class LookupChangeTest(FakeGhMixin):
    """Tests for lookup_change() and resolve_requirements()."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.init_repo('repo')
        os.chdir(self.repo)
        self.gh = str(self.install_fake_gh(self.repo))

    def test_number(self) -> None:
        self.add_view_response(
            'pigweed', 123, f'Title\n\nBody\n\nChange-Id: {_CHANGE_ID_A}\n'
        )
        self.assertEqual(
            requires.lookup_change(self.gh, Requirement('pigweed', number=123)),
            Requirement('pigweed', 123, _CHANGE_ID_A),
        )
        self.assertEqual(self.gh_calls(), [_view_args('pigweed', 123)])

    def test_number_uses_last_change_id(self) -> None:
        self.add_view_response(
            'pigweed',
            123,
            f'Title\n\nChange-Id: {_CHANGE_ID_B} in the body\n'
            f'Change-Id: {_CHANGE_ID_B}\n\nChange-Id: {_CHANGE_ID_A}\n',
        )
        result = requires.lookup_change(
            self.gh, Requirement('pigweed', number=123)
        )
        self.assertEqual(result.change_id, _CHANGE_ID_A)

    def test_number_without_change_id(self) -> None:
        self.add_view_response('pigweed', 123, 'Title\n\nNo footer.\n')
        with self.assertRaisesRegex(ValueError, 'no Change-Id footer'):
            requires.lookup_change(self.gh, Requirement('pigweed', number=123))

    def test_number_gh_failure(self) -> None:
        self.add_failure_response(
            _view_args('pigweed', 123), stderr='404 Not Found'
        )
        with self.assertRaisesRegex(ValueError, '404 Not Found'):
            requires.lookup_change(self.gh, Requirement('pigweed', number=123))

    def test_change_id_single_match(self) -> None:
        self.add_search_response('secret', _CHANGE_ID_A, [_search_result(55)])
        self.assertEqual(
            requires.lookup_change(
                self.gh, Requirement('secret', change_id=_CHANGE_ID_A)
            ),
            Requirement('secret', 55, _CHANGE_ID_A),
        )
        self.assertEqual(
            self.gh_calls(), [_search_args('secret', _CHANGE_ID_A)]
        )

    def test_change_id_no_match(self) -> None:
        self.add_search_response('secret', _CHANGE_ID_A, [])
        with self.assertRaisesRegex(ValueError, 'no changes found'):
            requires.lookup_change(
                self.gh, Requirement('secret', change_id=_CHANGE_ID_A)
            )

    def test_change_id_multiple_matches(self) -> None:
        self.add_search_response(
            'secret',
            _CHANGE_ID_A,
            [
                _search_result(55, branch='main'),
                _search_result(66, branch='release-1'),
                _search_result(77, branch='release-2'),
            ],
        )
        with self.assertRaises(ValueError) as ctx:
            requires.lookup_change(
                self.gh, Requirement('secret', change_id=_CHANGE_ID_A)
            )
        message = str(ctx.exception)
        self.assertIn('matches 3 changes', message)
        for number, branch in (
            (55, 'main'),
            (66, 'release-1'),
            (77, 'release-2'),
        ):
            self.assertIn(f'secret:{number}', message)
            self.assertIn(f'branch={branch}', message)

    def test_change_id_gh_failure(self) -> None:
        self.add_failure_response(
            _search_args('secret', _CHANGE_ID_A), stderr='403 Forbidden'
        )
        with self.assertRaisesRegex(ValueError, '403 Forbidden'):
            requires.lookup_change(
                self.gh, Requirement('secret', change_id=_CHANGE_ID_A)
            )

    def test_resolve_requirements(self) -> None:
        self.add_view_response(
            'pigweed', 123, f'T\n\nChange-Id: {_CHANGE_ID_A}'
        )
        self.add_search_response('secret', _CHANGE_ID_B, [_search_result(55)])
        self.assertEqual(
            requires.resolve_requirements(
                ['pigweed:123', f'secret:{_CHANGE_ID_B}']
            ),
            [
                Requirement('pigweed', 123, _CHANGE_ID_A),
                Requirement('secret', 55, _CHANGE_ID_B),
            ],
        )

    def test_resolve_requirements_removes_duplicates(self) -> None:
        self.add_view_response(
            'pigweed', 123, f'T\n\nChange-Id: {_CHANGE_ID_A}'
        )
        self.add_search_response('pigweed', _CHANGE_ID_A, [_search_result(123)])
        self.assertEqual(
            requires.resolve_requirements(
                ['pigweed:123', f'pigweed:{_CHANGE_ID_A}', 'pigweed:123']
            ),
            [Requirement('pigweed', 123, _CHANGE_ID_A)],
        )

    def test_resolve_requirements_parses_before_lookup(self) -> None:
        with self.assertRaises(ValueError):
            requires.resolve_requirements(['pigweed:123', 'invalid'])
        self.assertEqual(self.gh_calls(), [])


class CreateCommitTest(GitTestCase):
    """Tests for create_commit()."""

    def setUp(self) -> None:
        super().setUp()
        self.helper = self.init_repo('helper')

    def test_create_commit(self) -> None:
        change_id = requires.create_commit(
            self.helper,
            [
                Requirement('pigweed', 123, _CHANGE_ID_A),
                Requirement('secret', 55, _CHANGE_ID_B),
            ],
        )

        self.assertRegex(change_id, requires._CHANGE_ID_RE)
        self.assertEqual(
            self.show_json(self.helper, 'HEAD', 'patches.json'),
            [
                {'gerrit_name': 'pigweed', 'number': 123},
                {'gerrit_name': 'secret', 'number': 55},
            ],
        )
        self.assertEqual(
            self.subject(self.helper), f'{requires._DNS} {change_id[1:11]}'
        )
        self.assertEqual(
            self.trailers(self.helper),
            [
                ('Depends-on', f'pigweed:{_CHANGE_ID_A}'),
                ('Depends-on', f'secret:{_CHANGE_ID_B}'),
                ('Change-Id', change_id),
            ],
        )
        self.assertEqual(self.git(self.helper, 'status', '--porcelain'), '')

    def test_unique_change_ids(self) -> None:
        first = requires.create_commit(
            self.helper, [Requirement('pigweed', 123, _CHANGE_ID_A)]
        )
        second = requires.create_commit(
            self.helper, [Requirement('pigweed', 456, _CHANGE_ID_B)]
        )
        self.assertNotEqual(first, second)


class PushCommitTest(GitTestCase):
    """Tests for push_commit()."""

    def test_no_push(self) -> None:
        self.assertEqual(
            requires.push_commit(self.tmp, push=False),
            Change(requires.HELPER_GERRIT, 123456789),
        )

    def test_push(self) -> None:
        helper = self.init_helper_repo()
        local = self.tmp / 'local'
        self.git(self.tmp, 'clone', '--quiet', str(helper), str(local))
        self.git(local, 'commit', '--quiet', '--allow-empty', '-m', 'New')

        with mock.patch.object(requires, 'HELPER_REPO', str(helper)):
            change = requires.push_commit(local)

        self.assertEqual(
            change, Change(requires.HELPER_GERRIT, _PUSHED_HELPER_NUMBER)
        )
        self.assertEqual(self.subject(helper, 'refs/for/main'), 'New')

    def test_push_unexpected_output(self) -> None:
        helper = self.init_helper_repo(hook=False)
        local = self.tmp / 'local'
        self.git(self.tmp, 'clone', '--quiet', str(helper), str(local))

        with mock.patch.object(requires, 'HELPER_REPO', str(helper)):
            with self.assertRaisesRegex(ValueError, 'invalid output'):
                requires.push_commit(local)


class AmendExistingChangeTest(GitTestCase):
    """Tests for amend_existing_change()."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.init_repo(
            'repo',
            f'pw_foo: Do things\n\nBody text.\n\nBug: b/1\n'
            f'Change-Id: {_CHANGE_ID_C}',
        )
        os.chdir(self.repo)

    def test_creates_patches_json(self) -> None:
        requires.amend_existing_change(Change('pigweed-internal', 9))
        self.assertEqual(
            self.show_json(self.repo, 'HEAD', 'patches.json'),
            [{'gerrit_name': 'pigweed-internal', 'number': 9}],
        )
        self.assertEqual(
            self.git(self.repo, 'rev-list', '--count', 'HEAD'), '1\n'
        )

    def test_appends_to_patches_json(self) -> None:
        (self.repo / 'patches.json').write_text(
            json.dumps([{'gerrit_name': 'pigweed', 'number': 1}])
        )
        self.git(self.repo, 'add', 'patches.json')
        self.git(self.repo, 'commit', '--quiet', '--amend', '--no-edit')

        requires.amend_existing_change(Change('pigweed-internal', 9))
        self.assertEqual(
            self.show_json(self.repo, 'HEAD', 'patches.json'),
            [
                {'gerrit_name': 'pigweed', 'number': 1},
                {'gerrit_name': 'pigweed-internal', 'number': 9},
            ],
        )

    def test_without_change_id_keeps_message(self) -> None:
        before = self.message(self.repo)
        requires.amend_existing_change(Change('pigweed-internal', 9))
        self.assertEqual(self.message(self.repo), before)

    def test_adds_depends_on(self) -> None:
        requires.amend_existing_change(
            Change('pigweed-internal', 9), _CHANGE_ID_A
        )
        self.assertEqual(self.subject(self.repo), 'pw_foo: Do things')
        self.assertIn('Body text.', self.message(self.repo))
        self.assertEqual(
            self.trailers(self.repo),
            [
                ('Bug', 'b/1'),
                ('Change-Id', _CHANGE_ID_C),
                ('Depends-on', f'pigweed-internal:{_CHANGE_ID_A}'),
            ],
        )

    def test_depends_on_not_duplicated(self) -> None:
        for change_id in (_CHANGE_ID_A, _CHANGE_ID_A, _CHANGE_ID_B):
            requires.amend_existing_change(
                Change('pigweed-internal', 9), change_id
            )
        depends_on = [
            v for k, v in self.trailers(self.repo) if k == 'Depends-on'
        ]
        self.assertEqual(
            depends_on,
            [
                f'pigweed-internal:{_CHANGE_ID_A}',
                f'pigweed-internal:{_CHANGE_ID_B}',
            ],
        )


class RunTest(FakeGhMixin):
    """Tests for run() and main()."""

    def setUp(self) -> None:
        super().setUp()
        self.helper = self.init_helper_repo()
        helper_repo = mock.patch.object(
            requires, 'HELPER_REPO', str(self.helper)
        )
        helper_repo.start()
        self.addCleanup(helper_repo.stop)

        self.repo = self.init_repo(
            'repo', f'pw_foo: Do things\n\nChange-Id: {_CHANGE_ID_C}'
        )
        self.install_fake_gh(self.repo)
        os.chdir(self.repo)

    def test_run(self) -> None:
        """Creates and pushes a helper change and amends the current one."""
        self.add_view_response(
            'pigweed', 123, f'T\n\nChange-Id: {_CHANGE_ID_A}'
        )
        self.add_search_response('secret', _CHANGE_ID_B, [_search_result(55)])

        self.assertEqual(
            requires.run(['pigweed:123', f'secret:{_CHANGE_ID_B}']), 0
        )

        # The helper change was pushed with the requirements.
        helper_ref = 'refs/for/main'
        self.assertEqual(
            self.show_json(self.helper, helper_ref, 'patches.json'),
            [
                {'gerrit_name': 'pigweed', 'number': 123},
                {'gerrit_name': 'secret', 'number': 55},
            ],
        )
        helper_trailers = self.trailers(self.helper, helper_ref)
        self.assertEqual(
            helper_trailers[:2],
            [
                ('Depends-on', f'pigweed:{_CHANGE_ID_A}'),
                ('Depends-on', f'secret:{_CHANGE_ID_B}'),
            ],
        )
        self.assertEqual(helper_trailers[2][0], 'Change-Id')
        helper_change_id = helper_trailers[2][1]

        # The current change now depends on the helper change.
        self.assertEqual(
            self.show_json(self.repo, 'HEAD', 'patches.json'),
            [
                {
                    'gerrit_name': requires.HELPER_GERRIT,
                    'number': _PUSHED_HELPER_NUMBER,
                }
            ],
        )
        self.assertEqual(
            self.trailers(self.repo),
            [
                ('Change-Id', _CHANGE_ID_C),
                (
                    'Depends-on',
                    f'{requires.HELPER_GERRIT}:{helper_change_id}',
                ),
            ],
        )

    def test_run_no_push(self) -> None:
        self.add_view_response(
            'pigweed', 123, f'T\n\nChange-Id: {_CHANGE_ID_A}'
        )
        self.assertEqual(requires.run(['pigweed:123'], push=False), 0)
        self.assertEqual(self.git(self.helper, 'for-each-ref', 'refs/for'), '')
        self.assertEqual(
            self.show_json(self.repo, 'HEAD', 'patches.json'),
            [{'gerrit_name': requires.HELPER_GERRIT, 'number': 123456789}],
        )

    def test_run_ambiguous_change_id(self) -> None:
        self.add_search_response(
            'secret',
            _CHANGE_ID_B,
            [
                _search_result(55, branch='main'),
                _search_result(66, branch='release'),
            ],
        )
        head = self.git(self.repo, 'rev-parse', 'HEAD')

        with self.assertLogs(requires._LOG, logging.ERROR) as logs:
            self.assertNotEqual(requires.run([f'secret:{_CHANGE_ID_B}']), 0)

        output = '\n'.join(logs.output)
        self.assertIn('branch=main', output)
        self.assertIn('branch=release', output)
        self.assertEqual(self.git(self.repo, 'rev-parse', 'HEAD'), head)
        self.assertEqual(self.git(self.helper, 'for-each-ref', 'refs/for'), '')

    def test_run_invalid_requirement(self) -> None:
        head = self.git(self.repo, 'rev-parse', 'HEAD')
        with self.assertLogs(requires._LOG, logging.ERROR):
            self.assertNotEqual(requires.run(['invalid']), 0)
        self.assertEqual(self.git(self.repo, 'rev-parse', 'HEAD'), head)
        self.assertEqual(self.gh_calls(), [])

    def test_run_without_gh(self) -> None:
        if shutil.which('gh-ish'):
            self.skipTest('gh-ish is installed on PATH')
        (self.repo / 'gh').unlink()
        head = self.git(self.repo, 'rev-parse', 'HEAD')
        with self.assertLogs(requires._LOG, logging.ERROR) as logs:
            self.assertNotEqual(requires.run(['pigweed:123']), 0)
        self.assertIn("couldn't find", '\n'.join(logs.output))
        self.assertEqual(self.git(self.repo, 'rev-parse', 'HEAD'), head)

    def test_run_outside_repo(self) -> None:
        not_repo = self.tmp / 'not-repo'
        not_repo.mkdir()
        os.chdir(not_repo)
        with self.assertLogs(requires._LOG, logging.ERROR) as logs:
            self.assertNotEqual(requires.run(['pigweed:123']), 0)
        self.assertIn('repository not clean', '\n'.join(logs.output))
        self.assertEqual(self.gh_calls(), [])

    def test_main(self) -> None:
        self.add_failure_response(_view_args('pigweed', 123), stderr='nope')
        argv = ['requires', '--no-push', 'pigweed:123']
        with mock.patch.object(sys, 'argv', argv):
            with self.assertLogs(requires._LOG, logging.ERROR):
                self.assertNotEqual(requires.main(), 0)
        self.assertEqual(self.gh_calls(), [_view_args('pigweed', 123)])


if __name__ == '__main__':
    unittest.main()
