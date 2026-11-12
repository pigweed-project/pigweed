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
"""Sets up a Python 3 virtualenv for Pigweed."""

import contextlib
import datetime
import glob
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import stat
import tempfile
import time

# Grabbing datetime string once so it will always be the same for all GnTarget
# objects.
_DATETIME_STRING = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')

# pip's default socket timeout is 15 seconds. pip's own retry logic (--retries)
# only covers establishing a connection and receiving response headers. A read
# timeout while streaming a response body (e.g. a wheel or its metadata from
# files.pythonhosted.org) is not retried by pip versions older than 25.1, so a
# single stalled read fails the entire install. Use a more forgiving timeout
# unless the user has explicitly configured one in the environment.
_PIP_DEFAULT_TIMEOUT_SECONDS = '60'

# Number of attempts for commands that download packages from the network.
_NETWORK_COMMAND_ATTEMPTS = 3

# Delay between attempts, multiplied by the attempt number.
_NETWORK_RETRY_DELAY_SECONDS = 5

# Substrings in pip output that indicate a transient network failure, in which
# case simply rerunning the command is likely to succeed.
_TRANSIENT_NETWORK_ERRORS = (
    'ReadTimeoutError',
    'ConnectTimeoutError',
    'NewConnectionError',
    'IncompleteRead',
    'ProtocolError',
    'ConnectionResetError',
    'RemoteDisconnected',
    'Read timed out',
    'Connection reset by peer',
    'Temporary failure in name resolution',
)


def _is_windows() -> bool:
    return platform.system().lower() == 'windows'


class GnTarget:
    def __init__(self, val):
        self.directory, self.target = val.split('#', 1)
        self.name = '-'.join(
            (re.sub(r'\W+', '_', self.target).strip('_'), _DATETIME_STRING)
        )


def git_stdout(*args, **kwargs):
    """Run git, passing args as git params and kwargs to subprocess."""
    return (
        subprocess.check_output(['git'] + list(args), **kwargs).strip().decode()
    )


def git_repo_root(path='./'):
    """Find git repository root."""
    try:
        return git_stdout('-C', path, 'rev-parse', '--show-toplevel')
    except (subprocess.CalledProcessError, OSError):
        return None


class GitRepoNotFound(Exception):
    """Git repository not found."""


def _installed_packages(venv_python):
    cmd = (venv_python, '-m', 'pip', '--disable-pip-version-check', 'list')
    output = subprocess.check_output(cmd).splitlines()
    return set(x.split()[0].lower() for x in output[2:])


def _required_packages(requirements):
    packages = set()

    for req in requirements:
        with open(req, 'r') as ins:
            for line in ins:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                packages.add(line.split('=')[0])

    return packages


def _is_transient_network_failure(output: str) -> bool:
    """Returns whether command output indicates a transient network error."""
    return any(error in output for error in _TRANSIENT_NETWORK_ERRORS)


def _pip_network_env() -> dict[str, str]:
    """Returns a copy of os.environ with network-friendly pip settings."""
    environ = dict(os.environ)
    environ.setdefault('PIP_DEFAULT_TIMEOUT', _PIP_DEFAULT_TIMEOUT_SECONDS)
    return environ


def _wait_before_retry(attempt: int, attempts: int, out) -> None:
    delay = _NETWORK_RETRY_DELAY_SECONDS * attempt
    print(
        f'Transient network error detected (attempt {attempt}/{attempts}); '
        f'retrying in {delay} seconds.',
        file=out,
        flush=True,
    )
    time.sleep(delay)


def _check_call(args, retry_on_network_errors=False, **kwargs):
    stdout = kwargs.get('stdout', sys.stdout)
    attempts = _NETWORK_COMMAND_ATTEMPTS if retry_on_network_errors else 1

    for attempt in range(1, attempts + 1):
        with tempfile.TemporaryFile(mode='w+') as temp:
            try:
                kwargs['stdout'] = temp
                kwargs['stderr'] = subprocess.STDOUT
                # Don't dump the (large) environment into the log.
                logged_kwargs = {k: v for k, v in kwargs.items() if k != 'env'}
                print(args, logged_kwargs, file=temp)
                subprocess.check_call(args, **kwargs)
                return
            except subprocess.CalledProcessError:
                temp.seek(0)
                output = temp.read()
                stdout.write(output)
                if attempt < attempts and _is_transient_network_failure(output):
                    _wait_before_retry(attempt, attempts, stdout)
                    continue
                raise


def _check_call_logged(args, log_path, retry_on_network_errors=False, **kwargs):
    """Runs a command, writing its output to log_path.

    On failure, raises a CalledProcessError whose output is the full contents
    of log_path.
    """
    attempts = _NETWORK_COMMAND_ATTEMPTS if retry_on_network_errors else 1

    with open(log_path, 'w') as outs:
        print(args, file=outs)

    for attempt in range(1, attempts + 1):
        start = os.path.getsize(log_path)
        with open(log_path, 'a') as outs:
            kwargs['stdout'] = outs
            kwargs['stderr'] = outs
            returncode = subprocess.call(args, **kwargs)
        if returncode == 0:
            return

        with open(log_path, 'rb') as ins:
            ins.seek(start)
            output = ins.read().decode('utf-8', errors='replace')

        if attempt < attempts and _is_transient_network_failure(output):
            with open(log_path, 'a') as outs:
                _wait_before_retry(attempt, attempts, outs)
            continue

        with open(log_path, 'r', encoding='utf-8', errors='replace') as ins:
            raise subprocess.CalledProcessError(returncode, args, ins.read())


def _find_files_by_name(roots, name, allow_nesting=False):
    matches = []
    for root in roots:
        for dirpart, dirs, files in os.walk(root):
            if name in files:
                matches.append(os.path.join(dirpart, name))
                # If this directory is a match don't recurse inside it looking
                # for more matches.
                if not allow_nesting:
                    dirs[:] = []

            # Filter directories starting with . to avoid searching unnecessary
            # paths and finding files that should be hidden.
            dirs[:] = [d for d in dirs if not d.startswith('.')]
    return matches


def _parse_pyvenv_cfg(pyvenv_cfg: str) -> dict[str, str]:
    """Parses key = value pairs from a pyvenv.cfg file."""
    values: dict[str, str] = {}
    with open(pyvenv_cfg, 'r') as ins:
        for line in ins:
            if ' = ' in line:
                key, value = line.strip().split(' = ', 1)
                values[key] = value
    return values


def _version_matches(cfg_version: str, version: tuple[int, ...]) -> bool:
    """Returns True if cfg_version matches the leading components of version."""
    if not cfg_version:
        return False
    cfg_parts = cfg_version.split('.')
    version_parts = [str(part) for part in version]
    return (
        len(cfg_parts) <= len(version_parts)
        and cfg_parts == version_parts[: len(cfg_parts)]
    )


def _pyvenv_cfg_matches_python(
    pyvenv_values: dict[str, str],
    python: str,
    venv_path: str,
    version: tuple[int, ...],
) -> bool:
    """Returns True if pyvenv.cfg matches the target Python home and version."""
    pydir = os.path.dirname(python)
    home = pyvenv_values.get('home')
    if pydir != home and not pydir.startswith(venv_path):
        return False
    return _version_matches(pyvenv_values.get('version', ''), version)


def _venv_has_pip(venv_python: str) -> bool:
    """Returns True if venv_python exists and has a working pip module."""
    try:
        return (
            subprocess.call(
                [venv_python, '-m', 'pip', '--version'],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            == 0
        )
    except OSError:
        return False


def _needs_venv_setup(
    pyvenv_cfg: str,
    venv_python: str,
    python: str,
    venv_path: str,
    version: tuple[int, ...],
    *,
    system_packages: bool,
    full_envsetup: bool,
) -> bool:
    """Returns whether `python -m venv` needs to run."""
    if not os.path.exists(pyvenv_cfg):
        return True

    # Check if the python location and version used for the existing virtualenv
    # is the same as the python we're using. On non-Windows, delete the
    # existing virtualenv if it doesn't match so `venv --upgrade` starts clean.
    pyvenv_values = _parse_pyvenv_cfg(pyvenv_cfg)
    if not _pyvenv_cfg_matches_python(
        pyvenv_values, python, venv_path, version
    ):
        if not _is_windows():
            shutil.rmtree(venv_path)
        return True

    if full_envsetup:
        return True

    expected_system_packages = 'true' if system_packages else 'false'
    if (
        pyvenv_values.get('include-system-site-packages', '').lower()
        != expected_system_packages
    ):
        return True

    return not _venv_has_pip(venv_python)


def _make_venv_writable(venv_path: str, *, skip_gn_dir: bool = True) -> None:
    """Makes read-only files in venv_path writable."""
    # Virtual environments may contain read-only files (notably activate
    # scripts). `venv` calls will fail if they are not writeable.
    if not os.path.isdir(venv_path):
        return

    for root, dirs, files in os.walk(venv_path):
        if skip_gn_dir and root == venv_path and 'gn' in dirs:
            dirs.remove('gn')
        for file in files:
            path = os.path.join(root, file)
            mode = os.lstat(path).st_mode
            if not (stat.S_ISLNK(mode) or (mode & stat.S_IWRITE)):
                os.chmod(path, mode | stat.S_IWRITE)


def _compute_pip_digest(
    pip_install_args: list[str],
    pyvenv_cfg: str,
    constraints,
    requirements,
) -> str:
    """Computes a SHA-256 digest of inputs that affect pip installations."""
    hasher = hashlib.sha256()
    hasher.update('\0'.join(pip_install_args).encode())
    hasher.update(b'\0')
    if os.path.isfile(pyvenv_cfg):
        with open(pyvenv_cfg, 'rb') as ins:
            hasher.update(ins.read())
    hasher.update(b'\0')
    # Note: Files pulled in transitively via `-r` or `-c` lines inside a
    # requirements or constraint file are not included in this hash.
    for path in list(constraints or ()) + list(requirements or ()):
        hasher.update(path.encode())
        hasher.update(b'\0')
        if os.path.isfile(path):
            with open(path, 'rb') as ins:
                hasher.update(ins.read())
        hasher.update(b'\0')
    return hasher.hexdigest()


def _needs_gn_gen(
    build_dir: str,
    args_digest: str,
    *,
    venv_created: bool,
) -> bool:
    """Returns whether `gn gen` needs to run for build_dir."""
    if venv_created:
        return True

    build_ninja_path = os.path.join(build_dir, 'build.ninja')
    args_gn_path = os.path.join(build_dir, 'args.gn')
    args_hash_path = os.path.join(build_dir, 'args.gn.sha256')
    if not (
        os.path.isfile(build_ninja_path)
        and os.path.isfile(args_gn_path)
        and os.path.isfile(args_hash_path)
    ):
        return True

    with open(args_hash_path, 'r') as ins:
        return ins.read().strip() != args_digest


def _flatten(*items):
    """Yields items from a series of items and nested iterables."""

    for item in items:
        if isinstance(item, (list, tuple)):
            yield from _flatten(*item)
        else:
            yield item


def _python_version(python_path: str):
    """Returns the version (major, minor, rev) of the `python_path` binary."""
    # Prints values like "3.10.0"
    command = (
        python_path,
        '-c',
        'import sys; print(".".join(map(str, sys.version_info[:3])))',
    )
    version_str = (
        subprocess.check_output(command, stderr=subprocess.STDOUT)
        .strip()
        .decode()
    )
    return tuple(map(int, version_str.split('.')))


def install(  # pylint: disable=too-many-arguments,too-many-locals,too-many-statements
    project_root,
    venv_path,
    full_envsetup=True,
    requirements=None,
    constraints=None,
    pip_install_disable_cache=None,
    pip_install_find_links=None,
    pip_install_offline=None,
    pip_install_require_hashes=None,
    gn_args=(),
    gn_targets=(),
    gn_out_dir=None,
    python=sys.executable,
    env=None,
    system_packages=False,
    use_pinned_pip_packages=True,
):
    """Creates a venv and installs all packages in this Git repo.

    When `full_envsetup` is False, steps whose inputs have not changed are
    skipped:
    - `python -m venv` is skipped when `pyvenv.cfg` matches `python` (`home`,
      `version`, and `include-system-site-packages`) and `venv_python -m pip`
      works.
    - `pip install` is skipped when `<venv_path>/pip-requirements.sha256`
      matches the digest of `pip_install_args`, `pyvenv.cfg`, `constraints`,
      and `requirements`.
    - `gn gen` is skipped when `<build_dir>/build.ninja`, `args.gn`, and
      `args.gn.sha256` exist and `args.gn.sha256` matches the digest of the GN
      args.
    """

    version = _python_version(python)
    if version[0] != 3:
        print('=' * 60, file=sys.stderr)
        print('Unexpected Python version:', version, file=sys.stderr)
        print('=' * 60, file=sys.stderr)
        return False

    # The bin/ directory is called Scripts/ on Windows. Don't ask.
    venv_bin = os.path.join(venv_path, 'Scripts' if os.name == 'nt' else 'bin')
    venv_python = os.path.join(venv_bin, 'python')

    if env is not None:
        env.set('VIRTUAL_ENV', venv_path)
        env.prepend('PATH', venv_bin)
        env.clear('PYTHONHOME')
        env.clear('__PYVENV_LAUNCHER__')
        env_ctx = env
    else:
        env_ctx = contextlib.nullcontext

    pyvenv_cfg = os.path.join(venv_path, 'pyvenv.cfg')

    venv_created = _needs_venv_setup(
        pyvenv_cfg,
        venv_python,
        python,
        venv_path,
        version,
        system_packages=system_packages,
        full_envsetup=full_envsetup,
    )
    if venv_created:
        _make_venv_writable(venv_path, skip_gn_dir=not _is_windows())

        # On Mac sometimes the CIPD Python has __PYVENV_LAUNCHER__ set to
        # point to the system Python, which causes CIPD Python to create
        # virtualenvs that reference the system Python instead of the CIPD
        # Python. Clearing __PYVENV_LAUNCHER__ fixes that. See also pwbug/59.
        envcopy = os.environ.copy()
        if '__PYVENV_LAUNCHER__' in envcopy:
            del envcopy['__PYVENV_LAUNCHER__']

        # TODO(spang): Pass --upgrade-deps and remove pip & setuptools
        # upgrade below. This can only be done once the minimum python
        # version is at least 3.9.
        cmd = [python, '-m', 'venv']

        # Windows requires strange wizardry, and must follow symlinks
        # starting with 3.11.
        #
        # Without this, windows fails bootstrap trying to copy
        # "environment\cipd\packages\python\bin\venvlauncher.exe"
        #
        # This file doesn't exist in Python 3.11 on Windows and may be a bug
        # in venv. Pigweed already uses symlinks on Windows for the GN build,
        # so adding this option is not an issue.
        #
        # Further excitement is had when trying to update a virtual environment
        # that is created using symlinks under Windows.  `venv` will fail with
        # and error that the source and destination are the same file.  To work
        # around this, we run `venv` in `--clear` mode under Windows.
        if _is_windows():
            cmd += ['--clear', '--symlinks']
        else:
            cmd += ['--upgrade']

        cmd += ['--system-site-packages'] if system_packages else []
        cmd += [venv_path]
        _check_call(cmd, env=envcopy)

    pw_root = os.environ.get('PW_ROOT')
    if not pw_root and env is not None:
        pw_root = env.get('PW_ROOT')
    if not pw_root:
        pw_root = git_repo_root()
    if not pw_root:
        raise GitRepoNotFound()

    # Sometimes we get an error saying "Egg-link ... does not match
    # installed location". This gets around that. The egg-link files
    # all come from 'pw'-prefixed packages we installed with --editable.
    # Source: https://stackoverflow.com/a/48972085
    for egg_link in glob.glob(
        os.path.join(venv_path, 'lib/python*/site-packages/*.egg-link')
    ):
        os.unlink(egg_link)

    pip_install_args = []
    if pip_install_find_links:
        for package_dir in pip_install_find_links:
            pip_install_args.append('--find-links')
            with env_ctx():
                pip_install_args.append(os.path.expandvars(package_dir))
    if pip_install_require_hashes:
        pip_install_args.append('--require-hashes')
    if pip_install_offline:
        pip_install_args.append('--no-index')
    if pip_install_disable_cache:
        pip_install_args.append('--no-cache-dir')

    def pip_install(*args):
        args = list(_flatten(args))
        with env_ctx():
            cmd = (
                [
                    venv_python,
                    '-m',
                    'pip',
                    '--disable-pip-version-check',
                    'install',
                ]
                + pip_install_args
                + args
            )
            return _check_call(
                cmd, retry_on_network_errors=True, env=_pip_network_env()
            )

    constraint_args = []
    if constraints:
        constraint_args.extend(
            '--constraint={}'.format(constraint) for constraint in constraints
        )

    # TODO(tonymd): Remove this when projects have defined requirements.
    if (not requirements) and constraints:
        requirements = constraints

    pip_digest = _compute_pip_digest(
        pip_install_args, pyvenv_cfg, constraints, requirements
    )
    pip_hash_file = os.path.join(venv_path, 'pip-requirements.sha256')
    pip_hash_unchanged = False
    if not venv_created and os.path.isfile(pip_hash_file):
        with open(pip_hash_file, 'r') as ins:
            pip_hash_unchanged = ins.read().strip() == pip_digest

    if not pip_hash_unchanged:
        if os.path.isfile(pip_hash_file):
            os.unlink(pip_hash_file)
        pip_install(
            '--log',
            os.path.join(venv_path, 'pip-upgrade.log'),
            '--upgrade',
            'pip',
            'setuptools',
            'toml',  # Needed for pyproject.toml package installs.
            # Include wheel so pip installs can be done without build
            # isolation.
            'wheel',
            'pip-tools',
            constraint_args,
        )

        if requirements:
            requirement_args = []
            # Note: --no-build-isolation should be avoided for installing 3rd
            # party Python packages that use C/C++ extension modules.
            # https://setuptools.pypa.io/en/latest/userguide/ext_modules.html
            requirement_args.extend(
                '--requirement={}'.format(req) for req in requirements
            )
            combined_requirement_args = requirement_args + constraint_args
            pip_install(
                '--log',
                os.path.join(venv_path, 'pip-requirements.log'),
                combined_requirement_args,
            )

        with open(pip_hash_file, 'w') as outs:
            print(pip_digest, file=outs)

    def install_packages(gn_target):
        if gn_out_dir is None:
            build_dir = os.path.join(venv_path, 'gn')
        else:
            build_dir = gn_out_dir

        env_log = 'env-{}.log'.format(gn_target.name)
        env_log_path = os.path.join(venv_path, env_log)
        with open(env_log_path, 'w') as outs:
            for key, value in sorted(os.environ.items()):
                if key.upper().endswith('PATH'):
                    print(key, '=', file=outs)
                    # pylint: disable=invalid-name
                    for v in value.split(os.pathsep):
                        print('   ', v, file=outs)
                    # pylint: enable=invalid-name
                else:
                    print(key, '=', value, file=outs)

        args = list(gn_args)
        if not use_pinned_pip_packages:
            args.append('pw_build_PIP_CONSTRAINTS=[]')

        args.append('dir_pigweed="{}"'.format(pw_root))
        joined_args = ' '.join(args)
        args_digest = hashlib.sha256(joined_args.encode()).hexdigest()
        args_hash_path = os.path.join(build_dir, 'args.gn.sha256')

        if _needs_gn_gen(
            build_dir,
            args_digest,
            venv_created=venv_created,
        ):
            if os.path.isfile(args_hash_path):
                os.unlink(args_hash_path)
            gn_log = 'gn-gen-{}.log'.format(gn_target.name)
            gn_log_path = os.path.join(venv_path, gn_log)
            try:
                with open(gn_log_path, 'w') as outs:
                    gn_cmd = ['gn', 'gen', build_dir]
                    gn_cmd.append('--args={}'.format(joined_args))

                    print(gn_cmd, file=outs)
                    subprocess.check_call(
                        gn_cmd,
                        cwd=os.path.join(project_root, gn_target.directory),
                        stdout=outs,
                        stderr=outs,
                    )
            except subprocess.CalledProcessError as err:
                with open(gn_log_path, 'r') as ins:
                    raise subprocess.CalledProcessError(
                        err.returncode, err.cmd, ins.read()
                    )

            with open(args_hash_path, 'w') as outs:
                print(args_digest, file=outs)

        ninja_log = 'ninja-{}.log'.format(gn_target.name)
        ninja_log_path = os.path.join(venv_path, ninja_log)
        ninja_cmd = ['ninja', '-C', build_dir, '-v']
        ninja_cmd.append(gn_target.target)
        # The ninja build runs pip to install Python packages, which may hit
        # transient network errors. Rerunning ninja only reruns the failed
        # steps.
        _check_call_logged(
            ninja_cmd,
            ninja_log_path,
            retry_on_network_errors=True,
            env=_pip_network_env(),
        )

        with open(os.path.join(venv_path, 'pip-list.log'), 'w') as outs:
            subprocess.check_call(
                [
                    venv_python,
                    '-m',
                    'pip',
                    '--disable-pip-version-check',
                    'list',
                ],
                stdout=outs,
            )

    if gn_targets:
        with env_ctx():
            for gn_target in gn_targets:
                install_packages(gn_target)

    return True
