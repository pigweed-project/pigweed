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
"""Create transitive CLs for requirements on internal Gerrits.

This is only intended to be used by Googlers.

If the current CL needs to be tested alongside internal-project:1234 on an
internal project, but "internal-project" is something that can't be referenced
publicly, this automates creation of a CL on the pigweed-internal Gerrit that
references internal-project:1234 so the current commit effectively has a
requirement on internal-project:1234.

For more see http://go/pigweed-ci-cq-intro.
"""

import argparse
import dataclasses
import json
import logging
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Callable, IO, Sequence
import uuid

HELPER_GERRIT = 'pigweed-internal'
HELPER_PROJECT = 'requires-helper'
HELPER_REPO = 'sso://{}/{}'.format(HELPER_GERRIT, HELPER_PROJECT)

# Pass checks that look for "DO NOT ..." and block submission.
_DNS = ' '.join(
    (
        'DO',
        'NOT',
        'SUBMIT',
    )
)
# pylint: disable=line-too-long
# Subset of the output from pushing to Gerrit.
DEFAULT_OUTPUT = f'''
remote:
remote:   https://{HELPER_GERRIT}-review.git.corp.google.com/c/{HELPER_PROJECT}/+/123456789 {_DNS} [NEW]
remote:
'''.strip()
# pylint: enable=line-too-long

_CHANGE_ID_RE = re.compile(r'^I[0-9a-f]{40}$')
_CHANGE_ID_FOOTER_RE = re.compile(
    r'^Change-Id:\s*(I[0-9a-f]{40})\s*$', re.MULTILINE
)

_LOG = logging.getLogger(__name__)


@dataclasses.dataclass
class Change:
    gerrit_name: str
    number: int


@dataclasses.dataclass
class Requirement:
    """A change required by the current change.

    At least one of number and change_id is set. change_id includes the
    leading 'I'.
    """

    gerrit_name: str
    number: int | None = None
    change_id: str | None = None

    def __str__(self) -> str:
        return f'{self.gerrit_name}:{self.number or self.change_id}'


class EnhancedJSONEncoder(json.JSONEncoder):
    def default(self, o):
        if dataclasses.is_dataclass(o):
            return dataclasses.asdict(o)
        return super().default(o)


def dump_json_patches(obj: Sequence[Change], outs: IO):
    json.dump(obj, outs, indent=2, cls=EnhancedJSONEncoder)


def log_entry_exit(func: Callable) -> Callable:
    def wrapper(*args, **kwargs):
        _LOG.debug('entering %s()', func.__name__)
        _LOG.debug('args %r', args)
        _LOG.debug('kwargs %r', kwargs)
        try:
            res = func(*args, **kwargs)
            _LOG.debug('return value %r', res)
            return res
        except Exception as exc:
            _LOG.debug('exception %r', exc)
            raise
        finally:
            _LOG.debug('exiting %s()', func.__name__)

    return wrapper


@log_entry_exit
def parse_args() -> argparse.Namespace:
    """Creates an argument parser and parses arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'requirements',
        nargs='+',
        help=(
            'Requirements to be added ("<gerrit-name>:<cl-number>" or '
            '"<gerrit-name>:I<change-id>").'
        ),
    )
    parser.add_argument(
        '--push',
        action=argparse.BooleanOptionalAction,
        default=True,
        help=argparse.SUPPRESS,  # This option is only for debugging.
    )

    return parser.parse_args()


@log_entry_exit
def _run_command(*args, **kwargs) -> subprocess.CompletedProcess:
    kwargs.setdefault('capture_output', True)
    _LOG.debug('%s', args)
    _LOG.debug('%s', kwargs)
    res = subprocess.run(*args, **kwargs)
    _LOG.debug('%s', res.stdout)
    _LOG.debug('%s', res.stderr)
    res.check_returncode()
    return res


def _git_root() -> Path:
    return Path(
        _run_command(['git', 'rev-parse', '--show-toplevel'])
        .stdout.decode()
        .rstrip('\n')
    )


@log_entry_exit
def parse_requirement(req: str) -> Requirement:
    """Parses '<gerrit-name>:<cl-number>' or '<gerrit-name>:I<change-id>'."""
    gerrit_name, sep, value = req.partition(':')
    if gerrit_name and sep:
        if value.isdigit():
            return Requirement(gerrit_name, number=int(value))
        if _CHANGE_ID_RE.match(value):
            return Requirement(gerrit_name, change_id=value)
    raise ValueError(
        f'invalid requirement {req!r}, expected '
        "'<gerrit-name>:<cl-number>' or '<gerrit-name>:I<change-id>'"
    )


@log_entry_exit
def find_gh() -> str:
    """Returns the path to './gh' (gh-ish) for the current repository."""
    gh = _git_root() / 'gh'
    if gh.is_file() and os.access(gh, os.X_OK):
        return str(gh)
    gh_ish = shutil.which('gh-ish')
    if gh_ish:
        return gh_ish
    raise FileNotFoundError(
        f"couldn't find '{gh}' or 'gh-ish' to look up requirements"
    )


def _run_gh(gh: str, req: Requirement, *args: str) -> Any:
    """Runs a './gh' command against req's Gerrit host and parses its JSON."""
    host = f'{req.gerrit_name}-review.googlesource.com'
    cmd = [gh, '--host', host, *args]
    try:
        res = _run_command(cmd)
    except subprocess.CalledProcessError as exc:
        raise ValueError(
            f"couldn't look up {req} with '{' '.join(cmd)}':\n"
            f'{exc.stderr.decode()}'
        ) from exc
    return json.loads(res.stdout)


@log_entry_exit
def lookup_change(gh: str, req: Requirement) -> Requirement:
    """Looks up the CL number and Change-Id of a change using './gh'.

    Args:
        gh: Path to './gh'.
        req: The change to look up. Must have a number or a Change-Id.

    Returns a Requirement with both number and change_id set.

    Raises ValueError if the change can't be found, or if a Change-Id matches
    more than one change (e.g., cherry-picks to other branches).
    """
    _LOG.info('looking up %s', req)

    if req.number is not None:
        details = _run_gh(
            gh,
            req,
            *('pr', 'view', str(req.number)),
            *('--json', 'number,commitMessage'),
        )
        matches = _CHANGE_ID_FOOTER_RE.findall(details['commitMessage'])
        if not matches:
            raise ValueError(f'no Change-Id footer in {req}')
        return Requirement(req.gerrit_name, req.number, matches[-1])

    # 'pr view' can't handle Change-Ids that match multiple changes, so search
    # for all changes with this Change-Id.
    changes = _run_gh(
        gh,
        req,
        *('pr', 'list', '--all-projects', '--state', 'all'),
        *('--limit', '100', '--search', f'change:{req.change_id}'),
        *('--json', 'number,project,branch,status,title'),
    )
    if not changes:
        raise ValueError(f'no changes found matching {req}')
    if len(changes) > 1:
        lines = [f'{req} matches {len(changes)} changes:']
        for change in changes:
            lines.append(
                f'  {req.gerrit_name}:{change["number"]}'
                f'  project={change["project"]}'
                f'  branch={change["branch"]}'
                f'  status={change["status"]}'
                f'  {change["title"]}'
            )
        lines.append(f"Pass '{req.gerrit_name}:<cl-number>' instead.")
        raise ValueError('\n'.join(lines))
    return Requirement(req.gerrit_name, changes[0]['number'], req.change_id)


@log_entry_exit
def resolve_requirements(
    requirement_strings: Sequence[str],
) -> list[Requirement]:
    """Parses requirements and looks up their CL numbers and Change-Ids."""
    requirements = [parse_requirement(x) for x in requirement_strings]
    gh = find_gh()
    resolved: list[Requirement] = []
    for req in requirements:
        result = lookup_change(gh, req)
        if result in resolved:
            _LOG.info('skipping duplicate requirement %s', req)
            continue
        resolved.append(result)
    return resolved


@log_entry_exit
def check_status() -> bool:
    res = subprocess.run(['git', 'status'], capture_output=True)
    if res.returncode:
        _LOG.error('repository not clean, commit to suppress this warning')
        return False
    return True


@log_entry_exit
def clone(requires_dir: Path) -> None:
    _LOG.info('cloning helper repository into %s', requires_dir)
    _run_command(['git', 'clone', HELPER_REPO, '.'], cwd=requires_dir)


@log_entry_exit
def create_commit(
    requires_dir: Path, requirements: Sequence[Requirement]
) -> str:
    """Create a commit in the local tree with the given requirements.

    Each requirement is added to 'patches.json' and gets a 'Depends-on:'
    footer, so each must have both a number and a Change-Id.

    Returns the Change-Id of the created commit, including the leading 'I'.
    """
    change_id = 'I' + str(uuid.uuid4()).replace('-', '00')
    _LOG.debug('change_id %s', change_id)

    patches: list[Change] = []
    for req in requirements:
        assert req.number is not None and req.change_id is not None
        patches.append(Change(req.gerrit_name, req.number))

    path = requires_dir / 'patches.json'
    _LOG.debug('path %s', path)
    with open(path, 'w') as outs:
        dump_json_patches(patches, outs)
        outs.write('\n')

    _run_command(['git', 'add', path], cwd=requires_dir)

    commit_message = [f'{_DNS} {change_id[1:11]}', '']
    for req in requirements:
        commit_message.append(f'Depends-on: {req.gerrit_name}:{req.change_id}')
    commit_message.append(f'Change-Id: {change_id}')

    _LOG.debug('message %s', commit_message)
    _run_command(
        ['git', 'commit', '-m', '\n'.join(commit_message)],
        cwd=requires_dir,
    )

    # Not strictly necessary, only used for logging.
    _run_command(['git', 'show'], cwd=requires_dir)

    return change_id


@log_entry_exit
def push_commit(requires_dir: Path, push=True) -> Change:
    """Push a commit to the helper repository.

    Args:
        requires_dir: Local checkout of the helper repository.
        push: Whether to actually push or if this is a local-only test.

    Returns a Change object referencing the pushed commit.
    """

    output: str = DEFAULT_OUTPUT
    if push:
        res = _run_command(
            ['git', 'push', HELPER_REPO, '+HEAD:refs/for/main'],
            cwd=requires_dir,
        )
        output = res.stderr.decode()

    _LOG.debug('output: %s', output)
    regex = re.compile(
        f'^\\s*remote:\\s*'
        f'https://{HELPER_GERRIT}-review.(?:git.corp.google|googlesource).com/'
        f'c/{HELPER_PROJECT}/\\+/(?P<num>\\d+)\\s+',
        re.MULTILINE,
    )
    _LOG.debug('regex %r', regex)
    match = regex.search(output)
    if not match:
        raise ValueError(f"invalid output from 'git push': {output}")
    change_num = int(match.group('num'))
    _LOG.info('created %s change %s', HELPER_PROJECT, change_num)
    return Change(HELPER_GERRIT, change_num)


@log_entry_exit
def amend_existing_change(
    dependency: Change, dependency_change_id: str | None = None
) -> None:
    """Amend the current change to depend on the dependency

    Adds the dependency to 'patches.json' and, if dependency_change_id is
    given, adds a 'Depends-on:' footer to the commit message.

    Args:
        dependency: The change on which the top of the current checkout now
            depends.
        dependency_change_id: The Change-Id of the dependency, including the
            leading 'I'.
    """
    patches_json = _git_root() / 'patches.json'
    _LOG.info('%s %d', patches_json, os.path.isfile(patches_json))

    patches = []
    if os.path.isfile(patches_json):
        with open(patches_json, 'r') as ins:
            patches = json.load(ins)

    patches.append(dependency)
    with open(patches_json, 'w') as outs:
        dump_json_patches(patches, outs)
        outs.write('\n')
    _LOG.info('%s %d', patches_json, os.path.isfile(patches_json))

    _run_command(['git', 'add', patches_json])

    if not dependency_change_id:
        _run_command(['git', 'commit', '--amend', '--no-edit'])
        return

    message = _run_command(['git', 'log', '-1', '--format=%B']).stdout
    footer = f'Depends-on: {dependency.gerrit_name}:{dependency_change_id}'
    message = _run_command(
        [
            'git',
            'interpret-trailers',
            '--if-exists',
            'addIfDifferent',
            '--trailer',
            footer,
        ],
        input=message,
    ).stdout
    _run_command(['git', 'commit', '--amend', '--file', '-'], input=message)


def run(requirements: Sequence[str], push: bool = True) -> int:
    """Entry point for requires."""

    if not check_status():
        return -1

    # Parse requirements and look them up before creating anything.
    try:
        resolved = resolve_requirements(requirements)
    except (ValueError, FileNotFoundError) as exc:
        _LOG.error('%s', exc)
        return -1

    # Create directory for checking out helper repository.
    with tempfile.TemporaryDirectory() as requires_dir_str:
        requires_dir = Path(requires_dir_str)
        # Clone into helper repository.
        clone(requires_dir)
        # Make commit with requirements from command line.
        change_id = create_commit(requires_dir, resolved)
        # Push that commit and save its number.
        change = push_commit(requires_dir, push=push)
    # Add dependency on newly pushed commit on current commit.
    amend_existing_change(change, change_id)

    return 0


def main() -> int:
    return run(**vars(parse_args()))


if __name__ == '__main__':
    try:
        # If pw_cli is available, use it to initialize logs.
        from pw_cli import log

        log.install(logging.INFO)
    except ImportError:
        # If pw_cli isn't available, display log messages like a simple print.
        logging.basicConfig(format='%(message)s', level=logging.INFO)

    sys.exit(main())
