#!/usr/bin/env python
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
"""Installs and then runs cipd.

This script installs cipd in ./tools/ (if necessary) and then executes it,
passing through all arguments.

Must be tested with Python 2 and Python 3.
"""

import hashlib
import os
import platform
import ssl
import subprocess
import sys
import base64

try:
    import httplib  # type: ignore
except ImportError:
    import http.client as httplib  # type: ignore[no-redef]

try:
    import urlparse  # type: ignore
except ImportError:
    import urllib.parse as urlparse  # type: ignore[no-redef]

# Generated from the following command. May need to be periodically rerun.
# $ cipd ls infra/tools/cipd | perl -pe "s[.*/][];s/^/    '/;s/\s*$/',\n/;"
SUPPORTED_PLATFORMS = (
    'aix-ppc64',
    'linux-386',
    'linux-amd64',
    'linux-arm64',
    'linux-armv6l',
    'linux-mips64',
    'linux-mips64le',
    'linux-mipsle',
    'linux-ppc64',
    'linux-ppc64le',
    'linux-s390x',
    'mac-amd64',
    'mac-arm64',
    'windows-386',
    'windows-amd64',
)


class UnsupportedPlatform(Exception):
    pass


try:
    SCRIPT_DIR = os.path.dirname(__file__)
except NameError:  # __file__ not defined.
    try:
        SCRIPT_DIR = os.path.join(
            os.environ['PW_ROOT'],
            'pw_env_setup',
            'py',
            'pw_env_setup',
            'cipd_setup',
        )
    except KeyError:
        raise Exception('Environment variable PW_ROOT not set')

VERSION_FILE = os.path.join(SCRIPT_DIR, '.cipd_version')
DIGESTS_FILE = VERSION_FILE + '.digests'

# Put CIPD client in tools so that users can easily get it in their PATH.
CIPD_HOST = 'chrome-infra-packages.appspot.com'

try:
    PW_ROOT = os.environ['PW_ROOT']
except KeyError:
    try:
        with open(os.devnull, 'w') as outs:
            PW_ROOT = (
                subprocess.check_output(
                    ['git', 'rev-parse', '--show-toplevel'],
                    stderr=outs,
                )
                .strip()
                .decode('utf-8')
            )
    except subprocess.CalledProcessError:
        PW_ROOT = ''

# Get default install dir from environment since args cannot always be passed
# through this script (args are passed as-is to cipd).
if 'CIPD_PY_INSTALL_DIR' in os.environ:
    DEFAULT_INSTALL_DIR = os.environ['CIPD_PY_INSTALL_DIR']
elif PW_ROOT:
    DEFAULT_INSTALL_DIR = os.path.join(PW_ROOT, '.cipd')
else:
    DEFAULT_INSTALL_DIR = ''


def platform_normalized():
    """Normalize platform into format expected in CIPD paths."""

    try:
        os_name = platform.system().lower()
        return {
            'linux': 'linux',
            'mac': 'mac',
            'darwin': 'mac',
            'windows': 'windows',
        }[os_name]
    except KeyError:
        raise Exception('unrecognized os: {}'.format(os_name))


def arch_normalized():
    """Normalize arch into format expected in CIPD paths."""

    machine = platform.machine()
    if machine.startswith(('arm', 'aarch')):
        machine = machine.replace('aarch', 'arm')
        if machine == 'arm64':
            return machine
        return 'armv6l'
    if machine.endswith('64'):
        return 'amd64'
    if machine.endswith('86'):
        return '386'
    raise Exception('unrecognized arch: {}'.format(machine))


def platform_arch_normalized():
    return '{}-{}'.format(platform_normalized(), arch_normalized())


def user_agent():
    """Generate a user-agent based on the project name and current hash."""

    try:
        with open(os.devnull, 'w') as devnull:
            rev = subprocess.check_output(
                ['git', '-C', SCRIPT_DIR, 'rev-parse', 'HEAD'],
                stderr=devnull,
            ).strip()
    except subprocess.CalledProcessError:
        rev = '???'

    if isinstance(rev, bytes):
        rev = rev.decode()

    return 'pigweed-infra/tools/{}'.format(rev)


def actual_hash(path):
    """Hash the file at path and return it."""

    hasher = hashlib.sha256()
    with open(path, 'rb') as ins:
        hasher.update(ins.read())
    return hasher.hexdigest()


def expected_hash():
    """Pulls expected hash from digests file."""

    expected_plat = platform_arch_normalized()

    with open(DIGESTS_FILE, 'r') as ins:
        for line in ins:
            line = line.strip()
            if line.startswith('#') or not line:
                continue
            plat, hashtype, hashval = line.split()
            if hashtype == 'sha256' and plat == expected_plat:
                return hashval
    raise Exception('platform {} not in {}'.format(expected_plat, DIGESTS_FILE))


def https_connect_with_proxy(target_url):
    """Create HTTPSConnection with proxy support."""

    proxy_env = os.environ.get('HTTPS_PROXY') or os.environ.get('https_proxy')
    if proxy_env in (None, ''):
        conn = httplib.HTTPSConnection(target_url)
        return conn

    url = urlparse.urlparse(proxy_env)
    conn = httplib.HTTPSConnection(url.hostname, url.port)
    headers = {}
    if url.username and url.password:
        auth = '%s:%s' % (url.username, url.password)
        py_version = sys.version_info.major
        if py_version >= 3:
            headers['Proxy-Authorization'] = 'Basic ' + str(
                base64.b64encode(auth.encode()).decode()
            )
        else:
            headers['Proxy-Authorization'] = 'Basic ' + base64.b64encode(auth)
    conn.set_tunnel(target_url, 443, headers)
    return conn


_SSL_ERROR = """

Bootstrap: SSL error in Python when downloading CIPD client.
If using system Python try

    sudo pip install certifi

And if on the system Python on a Mac try

    /Applications/Python <version>/Install Certificates.command

If using Homebrew Python try

    brew install openssl
    brew uninstall python
    brew install python

If those don't work, address all the potential issues shown
by the following command.

    brew doctor

Otherwise, check that your machine's Python can use SSL,
testing with the httplib module on Python 2 or http.client on
Python 3.
"""


def client_bytes():
    """Pull down the CIPD client and return it as a bytes object.

    Often CIPD_HOST returns a 302 FOUND with a pointer to
    storage.googleapis.com, so this needs to handle redirects, but it
    shouldn't require the initial response to be a redirect either.
    """

    with open(VERSION_FILE, 'r') as ins:
        version = ins.read().strip()

    try:
        conn = https_connect_with_proxy(CIPD_HOST)
    except AttributeError:
        print('=' * 70, file=sys.stderr)
        print(_SSL_ERROR, file=sys.stderr)
        print('=' * 70, file=sys.stderr)
        raise

    full_platform = platform_arch_normalized()
    if full_platform not in SUPPORTED_PLATFORMS:
        raise UnsupportedPlatform(full_platform)

    path = '/client?platform={}&version={}'.format(full_platform, version)

    for _ in range(10):
        try:
            conn.request('GET', path)
            res = conn.getresponse()
            # Have to read the response before making a new request, so make
            # sure we always read it.
            content = res.read()
        except ssl.SSLError:
            print(_SSL_ERROR, file=sys.stderr)
            raise

        # Found client bytes.
        if res.status == httplib.OK:  # pylint: disable=no-else-return
            return content

        # Redirecting to another location.
        elif res.status == httplib.FOUND:
            location = res.getheader('location')
            url = urlparse.urlparse(location)
            if url.netloc != conn.host:
                conn = https_connect_with_proxy(url.netloc)
            path = '{}?{}'.format(url.path, url.query)

        # Some kind of error in this response.
        else:
            break

    raise Exception(
        'failed to download client from https://{}{}'.format(CIPD_HOST, path)
    )


def bootstrap(
    client,
    silent=('PW_ENVSETUP_QUIET' in os.environ),
):
    """Bootstrap cipd client installation."""

    client_dir = os.path.dirname(client)
    if not os.path.isdir(client_dir):
        os.makedirs(client_dir)

    if not silent:
        print(
            'Bootstrapping cipd client for {}'.format(
                platform_arch_normalized()
            )
        )

    tmp_path = client + '.tmp'
    with open(tmp_path, 'wb') as tmp:
        tmp.write(client_bytes())

    expected = expected_hash()
    actual = actual_hash(tmp_path)

    if expected != actual:
        raise Exception(
            'digest of downloaded CIPD client is incorrect, '
            'check that digests file is current'
        )

    os.chmod(tmp_path, 0o755)
    os.rename(tmp_path, client)


def selfupdate(client):
    """Update cipd client."""

    cmd = [
        client,
        'selfupdate',
        '-version-file',
        VERSION_FILE,
        '-service-url',
        'https://{}'.format(CIPD_HOST),
    ]
    subprocess.check_call(cmd)


def _default_client(install_dir):
    client = os.path.join(install_dir, 'cipd')
    if os.name == 'nt':
        client += '.exe'
    return client


def init(
    install_dir=DEFAULT_INSTALL_DIR,
    silent=False,
    client=None,
):
    """Install/update cipd client."""

    if not client:
        client = _default_client(install_dir)

    os.environ['CIPD_HTTP_USER_AGENT_PREFIX'] = user_agent()

    if not os.path.isfile(client):
        bootstrap(client, silent)

    try:
        selfupdate(client)
    except subprocess.CalledProcessError:
        print(
            'CIPD selfupdate failed. Bootstrapping then retrying...',
            file=sys.stderr,
        )
        bootstrap(client)
        selfupdate(client)

    return client


def main(install_dir=DEFAULT_INSTALL_DIR, silent=False):
    """Install/update cipd client."""

    client = _default_client(install_dir)

    try:
        init(install_dir=install_dir, silent=silent, client=client)

    except UnsupportedPlatform:
        # Don't show help message below for this exception.
        raise

    except Exception:
        print(
            'Failed to initialize CIPD. Run '
            '`CIPD_HTTP_USER_AGENT_PREFIX={user_agent}/manual {client} '
            "selfupdate -version-file '{version_file}'` "
            'to diagnose if this is persistent.'.format(
                user_agent=user_agent(),
                client=client,
                version_file=VERSION_FILE,
            ),
            file=sys.stderr,
        )
        raise

    return client


if __name__ == '__main__':
    client_exe = main()
    subprocess.check_call([client_exe] + sys.argv[1:])
