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
"""Tests for pw_env_setup.virtualenv_setup.install."""

# pylint: disable=protected-access

import importlib
import io
import os
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

# pw_env_setup.virtualenv_setup re-exports install.install(), which shadows the
# install module attribute, so import the module explicitly.
install = importlib.import_module('pw_env_setup.virtualenv_setup.install')

_TIMEOUT_OUTPUT = (
    'pip._vendor.urllib3.exceptions.ReadTimeoutError: '
    "HTTPSConnectionPool(host='files.pythonhosted.org', port=443): "
    'Read timed out.\n'
)
_RESOLUTION_OUTPUT = 'ERROR: ResolutionImpossible: conflicting dependencies\n'


def _fake_check_call(outputs):
    """Returns a check_call fake that emits outputs, failing if non-None."""
    remaining = list(outputs)

    def check_call(args, **kwargs):
        output = remaining.pop(0)
        if output is None:
            return 0
        kwargs['stdout'].write(output)
        raise subprocess.CalledProcessError(1, args)

    return check_call


def _fake_call(outputs):
    """Returns a subprocess.call fake that emits outputs, failing if set."""
    remaining = list(outputs)

    def call(unused_args, **kwargs):
        output = remaining.pop(0)
        if output is None:
            return 0
        kwargs['stdout'].write(output)
        return 1

    return call


@mock.patch.object(install.time, 'sleep')
class CheckCallTest(unittest.TestCase):
    """Tests for install._check_call."""

    def test_retries_transient_network_error(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT, None]),
        ) as check_call:
            install._check_call(
                ['pip', 'install'], retry_on_network_errors=True, stdout=out
            )
        self.assertEqual(check_call.call_count, 2)
        self.assertIn('ReadTimeoutError', out.getvalue())
        self.assertIn('retrying', out.getvalue())

    def test_gives_up_after_max_attempts(self, unused_sleep) -> None:
        out = io.StringIO()
        attempts = install._NETWORK_COMMAND_ATTEMPTS
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT] * attempts),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(
                    ['pip', 'install'], retry_on_network_errors=True, stdout=out
                )
        self.assertEqual(check_call.call_count, attempts)

    def test_does_not_retry_non_network_error(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_RESOLUTION_OUTPUT]),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(
                    ['pip', 'install'], retry_on_network_errors=True, stdout=out
                )
        self.assertEqual(check_call.call_count, 1)
        self.assertIn('ResolutionImpossible', out.getvalue())

    def test_does_not_retry_by_default(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT]),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(['pip', 'install'], stdout=out)
        self.assertEqual(check_call.call_count, 1)


@mock.patch.object(install.time, 'sleep')
class CheckCallLoggedTest(unittest.TestCase):
    """Tests for install._check_call_logged."""

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.log_path = os.path.join(self._tempdir.name, 'ninja.log')

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def test_retries_transient_network_error(self, unused_sleep) -> None:
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT, None]),
        ) as call:
            install._check_call_logged(
                ['ninja'], self.log_path, retry_on_network_errors=True
            )
        self.assertEqual(call.call_count, 2)
        with open(self.log_path) as ins:
            log = ins.read()
        self.assertIn("['ninja']", log)
        self.assertIn('ReadTimeoutError', log)
        self.assertIn('retrying', log)

    def test_raises_with_full_log(self, unused_sleep) -> None:
        attempts = install._NETWORK_COMMAND_ATTEMPTS
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT] * attempts),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError) as ctx:
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, attempts)
        self.assertEqual(
            ctx.exception.output.count('ReadTimeoutError'), attempts
        )

    def test_does_not_retry_non_network_error(self, unused_sleep) -> None:
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_RESOLUTION_OUTPUT]),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError) as ctx:
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, 1)
        self.assertIn('ResolutionImpossible', ctx.exception.output)

    def test_only_inspects_latest_attempt(self, unused_sleep) -> None:
        # A network error from an earlier attempt must not cause a later,
        # unrelated failure to be retried.
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT, _RESOLUTION_OUTPUT]),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, 2)


class PipNetworkEnvTest(unittest.TestCase):
    """Tests for install._pip_network_env."""

    def test_sets_default_timeout(self) -> None:
        with mock.patch.dict(os.environ, clear=True):
            env = install._pip_network_env()
        self.assertEqual(
            env['PIP_DEFAULT_TIMEOUT'], install._PIP_DEFAULT_TIMEOUT_SECONDS
        )

    def test_respects_existing_timeout(self) -> None:
        with mock.patch.dict(os.environ, {'PIP_DEFAULT_TIMEOUT': '5'}):
            env = install._pip_network_env()
        self.assertEqual(env['PIP_DEFAULT_TIMEOUT'], '5')


class _InstallTestBase(unittest.TestCase):
    """Common fixture for install.install() skip/rerun tests."""

    def setUp(self) -> None:
        tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(tempdir.cleanup)
        self.project_root = os.path.join(tempdir.name, 'project')
        pw_root = os.path.join(self.project_root, 'pigweed')
        self.venv_path = os.path.join(tempdir.name, 'venv')
        self.python = os.path.join(
            tempdir.name, 'cipd_python', 'bin', 'python3'
        )
        os.makedirs(pw_root)
        os.makedirs(os.path.dirname(self.python))

        self.req_file = os.path.join(self.project_root, 'requirements.txt')
        with open(self.req_file, 'w') as outs:
            outs.write('sample-pkg==1.0.0\n')

        self.constraint_file = os.path.join(
            self.project_root, 'constraints.list'
        )
        with open(self.constraint_file, 'w') as outs:
            outs.write('sample-pkg==1.0.0\n')

        env_patch = mock.patch.dict(
            os.environ, {'PW_ROOT': pw_root}, clear=False
        )
        env_patch.start()
        os.environ.pop('PW_ENVSETUP_FULL', None)
        self.addCleanup(env_patch.stop)

        self.python_version = self._enter_patch(
            mock.patch.object(
                install, '_python_version', return_value=(3, 11, 3)
            )
        )
        self.check_call = self._enter_patch(
            mock.patch.object(
                install, '_check_call', side_effect=self._fake_install_call
            )
        )
        self._enter_patch(mock.patch.object(install, '_check_call_logged'))
        self.subproc_check_call = self._enter_patch(
            mock.patch.object(
                install.subprocess,
                'check_call',
                side_effect=self._fake_subproc_check_call,
            )
        )
        self.subproc_call = self._enter_patch(
            mock.patch.object(install.subprocess, 'call', return_value=0)
        )

    def _enter_patch(self, patcher):
        started = patcher.start()
        self.addCleanup(patcher.stop)
        return started

    def _write_pyvenv_cfg(
        self,
        *,
        home: str = '',
        version: str = '3.11.3',
        system_packages: bool = False,
        extra_line: str = '',
    ) -> str:
        os.makedirs(self.venv_path, exist_ok=True)
        pyvenv_cfg = os.path.join(self.venv_path, 'pyvenv.cfg')
        sys_pkgs = 'true' if system_packages else 'false'
        home_dir = home or os.path.dirname(self.python)
        with open(pyvenv_cfg, 'w') as outs:
            outs.write(f'home = {home_dir}\n')
            outs.write(f'include-system-site-packages = {sys_pkgs}\n')
            outs.write(f'version = {version}\n')
            if extra_line:
                outs.write(f'{extra_line}\n')
        return pyvenv_cfg

    def _fake_install_call(self, args, **unused_kwargs) -> int:
        if list(args[1:3]) == ['-m', 'venv']:
            version_str = '.'.join(map(str, self.python_version.return_value))
            self._write_pyvenv_cfg(
                version=version_str,
                system_packages='--system-site-packages' in args,
            )
        return 0

    @staticmethod
    def _fake_subproc_check_call(args, **unused_kwargs) -> int:
        if list(args[:2]) == ['gn', 'gen']:
            build_dir = args[2]
            os.makedirs(build_dir, exist_ok=True)
            with open(os.path.join(build_dir, 'build.ninja'), 'w') as outs:
                outs.write('# generated build.ninja\n')
            # Simulate GN re-serializing list-valued args across multiple lines.
            with open(os.path.join(build_dir, 'args.gn'), 'w') as outs:
                outs.write('alpha = [\n  "//a",\n  "//b",\n]\n')
        return 0

    def _run_install(self, **kwargs) -> bool:
        kwargs.setdefault('full_envsetup', False)
        return install.install(
            project_root=self.project_root,
            venv_path=self.venv_path,
            python=self.python,
            **kwargs,
        )

    def _venv_calls(self) -> list[list[str]]:
        return [
            list(call.args[0])
            for call in self.check_call.call_args_list
            if list(call.args[0][1:3]) == ['-m', 'venv']
        ]

    def _pip_calls(self) -> list[list[str]]:
        return [
            list(call.args[0])
            for call in self.check_call.call_args_list
            if list(call.args[0][1:3]) == ['-m', 'pip']
        ]

    def _gn_gen_calls(self) -> list[list[str]]:
        return [
            list(call.args[0])
            for call in self.subproc_check_call.call_args_list
            if list(call.args[0][:2]) == ['gn', 'gen']
        ]


class VenvSetupTest(_InstallTestBase):
    """Tests for venv creation, skipping, repair, and chmod walk."""

    def test_install_creates_venv_when_pyvenv_cfg_missing(self) -> None:
        self._run_install()
        self.assertEqual(len(self._venv_calls()), 1)

    def test_install_skips_venv_when_pyvenv_cfg_present(self) -> None:
        self._write_pyvenv_cfg()
        self._run_install()
        self.assertEqual(self._venv_calls(), [])

    def test_install_runs_venv_when_full_envsetup_true(self) -> None:
        self._write_pyvenv_cfg()
        self._run_install(full_envsetup=True)
        self.assertEqual(len(self._venv_calls()), 1)

    def test_install_runs_venv_when_system_packages_toggled(self) -> None:
        self._write_pyvenv_cfg(system_packages=False)
        self._run_install(system_packages=True)
        venv_calls = self._venv_calls()
        self.assertEqual(len(venv_calls), 1)
        self.assertIn('--system-site-packages', venv_calls[0])

    def test_version_matches(self) -> None:
        cases = (
            ('3.11.10', True),
            ('3.11', True),
            ('3.11.1', False),
            ('3.11.10.1', False),
            ('', False),
        )
        for cfg_version, expected in cases:
            with self.subTest(cfg_version=cfg_version):
                self.assertIs(
                    install._version_matches(cfg_version, (3, 11, 10)),
                    expected,
                )

    def test_install_runs_venv_when_version_differs_on_windows(self) -> None:
        # Patch `_is_windows` so `_needs_venv_setup()` does not delete
        # `venv_path` and instead tests the in-place Windows recreate path.
        self._write_pyvenv_cfg(version='3.10.9')
        with mock.patch.object(install, '_is_windows', return_value=True):
            self._run_install()
        self.assertEqual(len(self._venv_calls()), 1)

    def test_install_runs_venv_when_home_differs_on_windows(self) -> None:
        self._write_pyvenv_cfg(home='/other/python/bin')
        with mock.patch.object(install, '_is_windows', return_value=True):
            self._run_install()
        self.assertEqual(len(self._venv_calls()), 1)

    def test_install_resolves_pw_root_from_git_when_env_is_none(self) -> None:
        self._write_pyvenv_cfg()
        os.environ.pop('PW_ROOT', None)
        with mock.patch.object(
            install, 'git_repo_root', return_value=self.project_root
        ) as mock_git_root:
            self.assertTrue(self._run_install(env=None))
        mock_git_root.assert_called_once_with()

    def test_install_repairs_venv_when_pip_missing(self) -> None:
        self._write_pyvenv_cfg()
        self.subproc_call.return_value = 1
        self._run_install()
        self.assertEqual(len(self._venv_calls()), 1)

    def test_chmod_walk_skips_gn_dir(self) -> None:
        bin_dir = os.path.join(self.venv_path, 'bin')
        gn_dir = os.path.join(self.venv_path, 'gn')
        os.makedirs(bin_dir)
        os.makedirs(gn_dir)

        activate_file = os.path.join(bin_dir, 'activate')
        gn_file = os.path.join(gn_dir, 'readonly.txt')
        for path in (activate_file, gn_file):
            with open(path, 'w') as outs:
                outs.write('content\n')
            os.chmod(path, stat.S_IREAD)
            self.addCleanup(os.chmod, path, stat.S_IREAD | stat.S_IWRITE)

        # pyvenv.cfg is missing so venv setup and the chmod walk run.
        with mock.patch.object(install, '_is_windows', return_value=False):
            self._run_install()

        self.assertTrue(os.lstat(activate_file).st_mode & stat.S_IWRITE)
        self.assertFalse(os.lstat(gn_file).st_mode & stat.S_IWRITE)

    def test_chmod_walk_includes_gn_dir_when_clearing_venv(self) -> None:
        gn_dir = os.path.join(self.venv_path, 'gn')
        os.makedirs(gn_dir)
        gn_file = os.path.join(gn_dir, 'readonly.txt')
        with open(gn_file, 'w') as outs:
            outs.write('content\n')
        os.chmod(gn_file, stat.S_IREAD)
        self.addCleanup(os.chmod, gn_file, stat.S_IREAD | stat.S_IWRITE)

        with mock.patch.object(install, '_is_windows', return_value=True):
            self._run_install()

        self.assertTrue(os.lstat(gn_file).st_mode & stat.S_IWRITE)

    def test_chmod_walk_not_run_for_existing_venv(self) -> None:
        self._write_pyvenv_cfg()
        bin_dir = os.path.join(self.venv_path, 'bin')
        os.makedirs(bin_dir)
        activate_file = os.path.join(bin_dir, 'activate')
        with open(activate_file, 'w') as outs:
            outs.write('content\n')
        os.chmod(activate_file, stat.S_IREAD)
        self.addCleanup(os.chmod, activate_file, stat.S_IREAD | stat.S_IWRITE)

        self._run_install()

        self.assertFalse(os.lstat(activate_file).st_mode & stat.S_IWRITE)


class PipInstallSkipTest(_InstallTestBase):
    """Tests for pip requirements hashing and skip/rerun behavior."""

    def test_pip_skipped_when_hash_matches(self) -> None:
        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.assertEqual(len(self._pip_calls()), 2)

        self.check_call.reset_mock()
        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.assertEqual(self._pip_calls(), [])

    def test_pip_reruns_when_requirements_file_contents_change(self) -> None:
        self._run_install(requirements=[self.req_file])
        self.check_call.reset_mock()

        with open(self.req_file, 'w') as outs:
            outs.write('sample-pkg==2.0.0\n')

        self._run_install(requirements=[self.req_file])
        self.assertEqual(len(self._pip_calls()), 2)

    def test_pip_reruns_when_constraint_file_contents_change(self) -> None:
        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.check_call.reset_mock()

        with open(self.constraint_file, 'w') as outs:
            outs.write('sample-pkg==1.1.0\n')

        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.assertEqual(len(self._pip_calls()), 2)

    def test_pip_reruns_when_pip_install_args_change(self) -> None:
        self._run_install(
            requirements=[self.req_file],
            pip_install_offline=False,
        )
        self.check_call.reset_mock()

        self._run_install(
            requirements=[self.req_file],
            pip_install_offline=True,
        )
        self.assertEqual(len(self._pip_calls()), 2)

    def test_pip_reruns_when_pyvenv_cfg_changes(self) -> None:
        self._run_install(requirements=[self.req_file])
        self.check_call.reset_mock()

        self._write_pyvenv_cfg(extra_line='prompt = custom')

        self._run_install(requirements=[self.req_file])
        self.assertEqual(len(self._pip_calls()), 2)

    def test_pip_reruns_when_hash_file_missing(self) -> None:
        self._run_install(requirements=[self.req_file])
        os.unlink(os.path.join(self.venv_path, 'pip-requirements.sha256'))
        self.check_call.reset_mock()

        self._run_install(requirements=[self.req_file])
        self.assertEqual(len(self._pip_calls()), 2)

    def test_pip_reruns_when_venv_created(self) -> None:
        self._run_install(requirements=[self.req_file])
        os.unlink(os.path.join(self.venv_path, 'pyvenv.cfg'))
        self.check_call.reset_mock()

        self._run_install(requirements=[self.req_file])
        self.assertEqual(len(self._venv_calls()), 1)
        self.assertEqual(len(self._pip_calls()), 2)

    def test_base_install_upgrade_flag(self) -> None:
        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.assertIn('--upgrade', self._pip_calls()[0])

        # Change requirements on an existing venv with constraints and confirm
        # base packages still pass --upgrade.
        with open(self.req_file, 'w') as outs:
            outs.write('sample-pkg==2.0.0\n')
        self.check_call.reset_mock()

        self._run_install(
            requirements=[self.req_file],
            constraints=[self.constraint_file],
        )
        self.assertIn('--upgrade', self._pip_calls()[0])

    def test_hash_not_written_when_pip_fails(self) -> None:
        hash_file = os.path.join(self.venv_path, 'pip-requirements.sha256')

        def fail_on_requirements(args, **kwargs):
            if any(str(arg).startswith('--requirement=') for arg in args):
                raise subprocess.CalledProcessError(1, args)
            return self._fake_install_call(args, **kwargs)

        self.check_call.side_effect = fail_on_requirements
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_install(requirements=[self.req_file])
        self.assertFalse(os.path.exists(hash_file))

    def test_hash_removed_when_pip_fails(self) -> None:
        hash_file = os.path.join(self.venv_path, 'pip-requirements.sha256')

        def fail_on_requirements(args, **kwargs):
            if any(str(arg).startswith('--requirement=') for arg in args):
                raise subprocess.CalledProcessError(1, args)
            return self._fake_install_call(args, **kwargs)

        self._run_install(requirements=[self.req_file])
        self.assertTrue(os.path.isfile(hash_file))

        with open(self.req_file, 'w') as outs:
            outs.write('sample-pkg==2.0.0\n')
        self.check_call.side_effect = fail_on_requirements
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_install(requirements=[self.req_file])
        self.assertFalse(os.path.exists(hash_file))


class GnGenSkipTest(_InstallTestBase):
    """Tests for gn gen triggers and args.gn.sha256 skip logic."""

    def setUp(self) -> None:
        super().setUp()
        self.gn_target = install.GnTarget(
            f'{self.project_root}#:python.install'
        )
        self.build_dir = os.path.join(self.venv_path, 'gn')

    def test_gn_gen_runs_when_venv_created(self) -> None:
        self._run_install(gn_targets=[self.gn_target])
        self.assertEqual(len(self._gn_gen_calls()), 1)

        # Deleting pyvenv.cfg forces venv creation, which must re-run gn gen
        # even when build.ninja, args.gn, and args.gn.sha256 are present.
        os.unlink(os.path.join(self.venv_path, 'pyvenv.cfg'))
        self.subproc_check_call.reset_mock()

        self._run_install(gn_targets=[self.gn_target])
        self.assertEqual(len(self._gn_gen_calls()), 1)

    def test_gn_gen_runs_when_required_file_missing(self) -> None:
        self._run_install(gn_targets=[self.gn_target])
        for filename in ('build.ninja', 'args.gn', 'args.gn.sha256'):
            with self.subTest(filename=filename):
                os.unlink(os.path.join(self.build_dir, filename))
                self.subproc_check_call.reset_mock()

                self._run_install(gn_targets=[self.gn_target])
                self.assertEqual(len(self._gn_gen_calls()), 1)

    def test_gn_gen_runs_when_args_differ(self) -> None:
        self._run_install(
            gn_targets=[self.gn_target],
            gn_args=['alpha=["//a"]'],
        )
        self.subproc_check_call.reset_mock()

        self._run_install(
            gn_targets=[self.gn_target],
            gn_args=['alpha=["//a", "//b"]'],
        )
        self.assertEqual(len(self._gn_gen_calls()), 1)

    def test_args_hash_removed_when_gn_gen_fails(self) -> None:
        args_hash_path = os.path.join(self.build_dir, 'args.gn.sha256')
        self._run_install(
            gn_targets=[self.gn_target],
            gn_args=['alpha=["//a"]'],
        )
        self.assertTrue(os.path.isfile(args_hash_path))

        def fail_on_gn_gen(args, **kwargs):
            if list(args[:2]) == ['gn', 'gen']:
                raise subprocess.CalledProcessError(1, args)
            return self._fake_subproc_check_call(args, **kwargs)

        self.subproc_check_call.side_effect = fail_on_gn_gen
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_install(
                gn_targets=[self.gn_target],
                gn_args=['alpha=["//a", "//b"]'],
            )
        self.assertFalse(os.path.exists(args_hash_path))

    def test_gn_gen_skipped_when_args_match(self) -> None:
        gn_args = ['alpha=["//a", "//b"]', 'enable_feature=true']
        self._run_install(
            gn_targets=[self.gn_target],
            gn_args=gn_args,
        )
        self.assertEqual(len(self._gn_gen_calls()), 1)

        self.subproc_check_call.reset_mock()
        self._run_install(
            gn_targets=[self.gn_target],
            gn_args=gn_args,
        )
        self.assertEqual(self._gn_gen_calls(), [])


if __name__ == '__main__':
    unittest.main()
