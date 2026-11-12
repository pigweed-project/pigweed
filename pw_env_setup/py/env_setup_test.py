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
"""Tests for pw_env_setup.env_setup."""

# pylint: disable=protected-access

import io
import json
import os
import tempfile
import unittest
from unittest import mock

from pw_env_setup import env_setup


class EnvSetupWriteGniTest(unittest.TestCase):
    """Tests for EnvSetup._write_gni_file and EnvSetup.virtualenv."""

    def setUp(self) -> None:
        env_patch = mock.patch.dict(os.environ, {}, clear=False)
        env_patch.start()
        self.addCleanup(env_patch.stop)
        os.environ.pop('PW_PACKAGE_ROOT', None)
        os.environ.pop('PW_ENVSETUP_FULL', None)

        self._tempdir = tempfile.TemporaryDirectory()
        self.project_root = os.path.join(self._tempdir.name, 'project')
        self.pw_root = os.path.join(self.project_root, 'pigweed')
        self.install_dir = os.path.join(self.project_root, 'environment')
        self.shell_file = os.path.join(self.install_dir, 'activate.sh')
        self.config_file = os.path.join(self.project_root, 'pigweed.json')

        os.makedirs(os.path.join(self.project_root, 'build_overrides'))
        os.makedirs(os.path.join(self.pw_root, 'build_overrides'))
        os.makedirs(self.install_dir)

        self.pigweed_gni_file = os.path.join(
            self.pw_root, 'build_overrides', 'pigweed_environment.gni'
        )
        with open(self.pigweed_gni_file, 'w') as outs:
            outs.write('# pigweed gni template\n')

        self.project_gni_file = os.path.join(
            self.project_root, 'build_overrides', 'pigweed_environment.gni'
        )
        self.generated_gni_file = os.path.join(
            self.install_dir, 'build_overrides', 'pigweed_environment.gni'
        )

        req_file = os.path.join(self.project_root, 'requirements.txt')
        with open(req_file, 'w') as outs:
            outs.write('sample-pkg==1.0.0\n')

        config = {
            'pw': {
                'pw_env_setup': {
                    'relative_pigweed_root': 'pigweed',
                    'virtualenv': {
                        'requirements': ['requirements.txt'],
                    },
                }
            }
        }
        with open(self.config_file, 'w') as outs:
            json.dump(config, outs)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def _make_env_setup(self) -> env_setup.EnvSetup:
        return env_setup.EnvSetup(
            pw_root=self.pw_root,
            cipd_cache_dir=None,
            shell_file=self.shell_file,
            quiet=True,
            install_dir=self.install_dir,
            strict=False,
            virtualenv_gn_out_dir=None,
            json_file=None,
            project_root=self.project_root,
            config_file=self.config_file,
            use_existing_cipd=False,
            check_submodules=False,
            use_pinned_pip_packages=True,
            cipd_only=False,
            trust_cipd_hash=False,
            additional_cipd_file=None,
        )

    def test_write_gni_includes_virtual_env_and_package_root(self) -> None:
        setup = self._make_env_setup()
        setup._write_gni_file()

        with open(self.generated_gni_file, 'r') as ins:
            contents = ins.read()
        self.assertIn('pw_env_setup_VIRTUAL_ENV =', contents)
        self.assertIn('pw_env_setup_PACKAGE_ROOT =', contents)

    def test_write_gni_does_not_mutate_env(self) -> None:
        setup = self._make_env_setup()
        json_before = io.StringIO()
        setup._env.json(json_before)

        setup._write_gni_file()

        json_after = io.StringIO()
        setup._env.json(json_after)
        self.assertEqual(json_after.getvalue(), json_before.getvalue())
        self.assertIsNone(setup._env.get('PW_PACKAGE_ROOT'))

    def test_write_gni_unchanged_after_pw_package(self) -> None:
        setup = self._make_env_setup()
        setup._write_gni_file()

        past_ts = 1_000_000
        os.utime(self.generated_gni_file, (past_ts, past_ts))
        mtime_before = os.stat(self.generated_gni_file).st_mtime_ns

        setup.pw_package(None)
        self.assertEqual(
            setup._env.get('PW_PACKAGE_ROOT'),
            os.path.join(self.install_dir, 'packages'),
        )
        setup._write_gni_file()

        self.assertEqual(
            os.stat(self.generated_gni_file).st_mtime_ns, mtime_before
        )

    def test_write_gni_first_write(self) -> None:
        log_dir = os.path.join(self.install_dir, 'logs')
        os.makedirs(log_dir)
        self.assertFalse(os.path.exists(self.generated_gni_file))

        setup = self._make_env_setup()
        setup._write_gni_file()

        self.assertTrue(os.path.isfile(self.generated_gni_file))
        with open(self.generated_gni_file, 'r') as ins:
            contents = ins.read()
        self.assertIn('declare_args() {', contents)
        self.assertIn('pw_env_setup_VIRTUAL_ENV =', contents)
        self.assertIn('pw_env_setup_PACKAGE_ROOT =', contents)

        logged_gni = os.path.join(log_dir, 'pigweed_environment.gni')
        self.assertTrue(os.path.isfile(logged_gni))
        with open(logged_gni, 'r') as ins:
            self.assertEqual(ins.read(), contents)

    def test_write_gni_unchanged_does_not_rewrite(self) -> None:
        setup = self._make_env_setup()
        setup._write_gni_file()

        past_ts = 1_000_000
        os.utime(self.generated_gni_file, (past_ts, past_ts))
        mtime_before = os.stat(self.generated_gni_file).st_mtime_ns

        setup._write_gni_file()

        self.assertEqual(
            os.stat(self.generated_gni_file).st_mtime_ns, mtime_before
        )

    def test_write_gni_changed_rewrites(self) -> None:
        setup = self._make_env_setup()
        setup._write_gni_file()

        past_ts = 1_000_000
        os.utime(self.generated_gni_file, (past_ts, past_ts))
        mtime_before = os.stat(self.generated_gni_file).st_mtime_ns

        setup._env.set(
            'PW_PIGWEED_CIPD_INSTALL_DIR',
            os.path.join(self.install_dir, 'cipd', 'pigweed'),
        )
        setup._write_gni_file()

        self.assertGreater(
            os.stat(self.generated_gni_file).st_mtime_ns, mtime_before
        )
        with open(self.generated_gni_file, 'r') as ins:
            self.assertIn('pw_env_setup_CIPD_PIGWEED =', ins.read())

    def test_write_gni_logs_dir_missing(self) -> None:
        log_dir = os.path.join(self.install_dir, 'logs')
        self.assertFalse(os.path.exists(log_dir))

        setup = self._make_env_setup()
        setup._write_gni_file()

        self.assertTrue(os.path.isfile(self.generated_gni_file))
        self.assertFalse(os.path.exists(log_dir))

    def test_project_gni_copied_when_missing(self) -> None:
        self.assertFalse(os.path.exists(self.project_gni_file))

        setup = self._make_env_setup()
        setup._write_gni_file()

        self.assertTrue(os.path.isfile(self.project_gni_file))
        with open(self.project_gni_file, 'r') as ins:
            self.assertEqual(ins.read(), '# pigweed gni template\n')

    def test_project_gni_not_copied_when_identical(self) -> None:
        setup = self._make_env_setup()
        setup._write_gni_file()

        past_ts = 1_000_000
        os.utime(self.project_gni_file, (past_ts, past_ts))
        mtime_before = os.stat(self.project_gni_file).st_mtime_ns

        setup._write_gni_file()

        self.assertEqual(
            os.stat(self.project_gni_file).st_mtime_ns, mtime_before
        )

    def test_project_gni_copied_when_different(self) -> None:
        with open(self.project_gni_file, 'w') as outs:
            outs.write('# stale project gni\n')

        setup = self._make_env_setup()
        setup._write_gni_file()

        with open(self.project_gni_file, 'r') as ins:
            self.assertEqual(ins.read(), '# pigweed gni template\n')

    def test_virtualenv_full_envsetup_false_by_default(self) -> None:
        setup = self._make_env_setup()
        with (
            mock.patch.object(
                env_setup, '_which', return_value='/usr/bin/python3'
            ),
            mock.patch.object(
                env_setup.virtualenv_setup, 'install', return_value=True
            ) as mock_install,
        ):
            setup.virtualenv(None)
        self.assertIs(mock_install.call_args.kwargs['full_envsetup'], False)

    def test_virtualenv_full_envsetup_true_when_env_var_set(self) -> None:
        os.environ['PW_ENVSETUP_FULL'] = '1'
        setup = self._make_env_setup()
        with (
            mock.patch.object(
                env_setup, '_which', return_value='/usr/bin/python3'
            ),
            mock.patch.object(
                env_setup.virtualenv_setup, 'install', return_value=True
            ) as mock_install,
        ):
            setup.virtualenv(None)
        self.assertIs(mock_install.call_args.kwargs['full_envsetup'], True)


if __name__ == '__main__':
    unittest.main()
