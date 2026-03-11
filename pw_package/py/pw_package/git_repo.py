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
"""Install and check status of Git repository-based packages."""

import logging
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import urllib.parse

import pw_package.package_manager

_LOG: logging.Logger = logging.getLogger(__name__)

_GIT_CONFIG = [
    # Suppress "You are in 'detached HEAD' state" message
    '-c',
    'advice.detachedHead=false',
]


def git_stdout(
    *args: Path | str, show_stderr=False, repo: Path | str = '.'
) -> str:
    _LOG.debug('executing %r in %r', args, repo)
    try:
        cmd = ['git'] + _GIT_CONFIG + ['-C', repo, *args]
        return (
            subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=None if show_stderr else subprocess.STDOUT,
                check=True,
            )
            .stdout.decode()
            .strip()
        )
    except subprocess.CalledProcessError as err:
        _LOG.error('Git command failed: %s', shlex.join(str(s) for s in cmd))
        if err.output:
            _LOG.error(
                'Git output:\n%s', err.output.decode('utf-8', errors='replace')
            )
        raise


def git(
    *args: Path | str, repo: Path | str = '.'
) -> subprocess.CompletedProcess:
    _LOG.debug('executing %r in %r', args, repo)
    return subprocess.run(
        ['git'] + _GIT_CONFIG + ['-C', repo, *args], check=True
    )


class GitRepo(pw_package.package_manager.Package):
    """Install and check status of Git repository-based packages."""

    def __init__(
        self, url, *args, commit='', tag='', sparse_list=None, **kwargs
    ):
        super().__init__(*args, **kwargs)
        if not (commit or tag):
            raise ValueError('git repo must specify a commit or tag')

        self._url = url
        self._commit = commit
        self._tag = tag
        self._sparse_list = sparse_list
        self._allow_use_in_downstream = False

    def status(self, path: Path) -> bool:
        _LOG.debug('%s: status', self.name)
        # TODO(tonymd): Check the correct SHA is checked out here.
        if not os.path.isdir(path / '.git'):
            _LOG.debug('%s: no .git folder', self.name)
            return False

        remote = git_stdout('remote', 'get-url', 'origin', repo=path)
        url = urllib.parse.urlparse(remote)
        if url.scheme == 'sso' or '.git.corp.google.com' in url.netloc:
            host = url.netloc.replace(
                '.git.corp.google.com',
                '.googlesource.com',
            )
            if not host.endswith('.googlesource.com'):
                host += '.googlesource.com'
            remote = 'https://{}{}'.format(host, url.path)
        if remote != self._url:
            _LOG.debug(
                "%s: remote doesn't match expected %s actual %s",
                self.name,
                self._url,
                remote,
            )
            return False

        commit = git_stdout('rev-parse', 'HEAD', repo=path)
        if self._commit and self._commit != 'HEAD' and self._commit != commit:
            _LOG.debug(
                "%s: commits don't match expected %s actual %s",
                self.name,
                self._commit,
                commit,
            )
            return False

        if self._tag:
            tag = git_stdout('describe', '--tags', repo=path)
            if self._tag != tag:
                _LOG.debug(
                    "%s: tags don't match expected %s actual %s",
                    self.name,
                    self._tag,
                    tag,
                )
                return False

        # If it is a sparse checkout, sparse list shall match.
        if self._sparse_list:
            if not self.check_sparse_list(path):
                _LOG.debug("%s: sparse lists don't match", self.name)
                return False

        status = git_stdout('status', '--porcelain=v1', repo=path)
        _LOG.debug('%s: status %r', self.name, status)
        return not status

    def install(self, path: Path) -> None:
        _LOG.debug('%s: install', self.name)
        # If already installed and at correct version exit now.
        if self.status(path):
            _LOG.debug('%s: already installed, exiting', self.name)
            return

        # Otherwise delete current version and clone again.
        if os.path.isdir(path):
            _LOG.debug('%s: removing', self.name)
            shutil.rmtree(path)

        if self._sparse_list:
            self.checkout_sparse(path)
        else:
            self.checkout_full(path)

    def checkout_full(self, path: Path) -> None:
        # --filter=blob:none means we don't get history, just the current
        # revision. If we later run commands that need history it will be
        # retrieved on-demand. For small repositories the effect is negligible
        # but for large repositories this should be a significant improvement.
        # --filter=... causes progress messages to be printed to stderr even if
        # using --quiet so we wrap our clone command in `get_stdout` to prevent
        # the output from being emitted.
        _LOG.debug('%s: checkout_full', self.name)
        if self._commit:
            git_stdout('clone', '--filter=blob:none', self._url, path)
            git('reset', '--hard', self._commit, repo=path)
        elif self._tag:
            git_stdout(
                'clone', '-b', self._tag, '--filter=blob:none', self._url, path
            )

    def checkout_sparse(self, path: Path) -> None:
        _LOG.debug('%s: checkout_sparse', self.name)
        # sparse checkout
        git('init', path)
        git('remote', 'add', 'origin', self._url, repo=path)
        git('config', 'core.sparseCheckout', 'true', repo=path)

        # Add files to checkout by editing .git/info/sparse-checkout
        with open(path / '.git' / 'info' / 'sparse-checkout', 'w') as sparse:
            for source in self._sparse_list:
                sparse.write(source + '\n')

        # Either pull from a commit or a tag.
        target = self._commit if self._commit else self._tag
        git('pull', '--depth=1', 'origin', target, repo=path)

    def check_sparse_list(self, path: Path) -> bool:
        sparse_list = (
            git_stdout('sparse-checkout', 'list', repo=path)
            .strip('\n')
            .splitlines()
        )
        return set(sparse_list) == set(self._sparse_list)
