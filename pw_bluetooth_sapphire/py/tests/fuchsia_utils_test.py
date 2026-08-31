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
"""Tests for bluetooth CLI integration."""

import argparse
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import call, patch, MagicMock

from pw_bluetooth import cli
from pw_bluetooth_sapphire import fuchsia_utils


class TestBluetoothCli(unittest.TestCase):
    """Tests for the Bluetooth CLI wrapper."""

    def test_parser_bt_host_deploy_with_checkout(self):
        # Test with explicit fuchsia_checkout argument
        with patch(
            'sys.argv', ['pw bluetooth', 'bt-host-deploy', '/tmp/fuchsia']
        ):
            with patch(
                'pw_bluetooth_sapphire.fuchsia_utils.bt_host_deploy',
                return_value=0,
            ) as mock_deploy:
                with patch('os.environ.get', return_value='/root'):
                    with patch.object(Path, 'exists', return_value=True):
                        with patch('sys.exit') as mock_exit:
                            cli.main()
                            mock_deploy.assert_called_once()
                            args, _ = mock_deploy.call_args
                            self.assertEqual(
                                args[0].fuchsia_checkout, Path('/tmp/fuchsia')
                            )
                            self.assertFalse(args[0].local_fuchsia_sdk)
                            mock_exit.assert_called_with(0)

    def test_parser_bt_host_deploy_with_local_sdk(self):
        # Test with --local-fuchsia-sdk flag
        with patch(
            'sys.argv',
            [
                'pw bluetooth',
                'bt-host-deploy',
                '--local-fuchsia-sdk',
                '/tmp/fuchsia',
            ],
        ):
            with patch(
                'pw_bluetooth_sapphire.fuchsia_utils.bt_host_deploy',
                return_value=0,
            ) as mock_deploy:
                with patch('os.environ.get', return_value='/root'):
                    with patch.object(Path, 'exists', return_value=True):
                        with patch('sys.exit') as mock_exit:
                            cli.main()
                            mock_deploy.assert_called_once()
                            args, _ = mock_deploy.call_args
                            self.assertEqual(
                                args[0].fuchsia_checkout, Path('/tmp/fuchsia')
                            )
                            self.assertTrue(args[0].local_fuchsia_sdk)
                            mock_exit.assert_called_with(0)

    def test_parser_bt_host_deploy_no_args(self):
        # Test with no arguments (uses config)
        with patch('sys.argv', ['pw bluetooth', 'bt-host-deploy']):
            with patch(
                'pw_bluetooth_sapphire.fuchsia_utils.bt_host_deploy',
                return_value=0,
            ) as mock_deploy:
                with patch('os.environ.get', return_value='/root'):
                    with patch.object(Path, 'exists', return_value=True):
                        with patch('sys.exit') as mock_exit:
                            cli.main()
                            mock_deploy.assert_called_once()
                            args, _ = mock_deploy.call_args
                            self.assertIsNone(args[0].fuchsia_checkout)
                            mock_exit.assert_called_with(0)

    @patch('pw_bluetooth.preferences.BluetoothPrefs.set_fuchsia_checkout_path')
    def test_set_fuchsia_checkout(
        self, mock_set
    ):  # pylint: disable=no-self-use
        # Test the set-fuchsia-checkout subcommand
        with patch(
            'sys.argv', ['pw bluetooth', 'set-fuchsia-checkout', '/new/path']
        ):
            with patch('sys.exit') as mock_exit:
                cli.main()
                mock_set.assert_called_once_with(Path('/new/path'))
                mock_exit.assert_called_with(0)

    @patch(
        'pw_bluetooth_sapphire.fuchsia_utils.get_project_root',
        return_value=Path('/tmp/project_root'),
    )
    @patch('shutil.copy')
    @patch(
        'pw_bluetooth_sapphire.fuchsia_utils.get_sha256',
        return_value='fake_sha',
    )
    @patch('pw_bluetooth_sapphire.fuchsia_utils.make_writable')
    @patch('pathlib.Path.exists', return_value=True)
    @patch('subprocess.run')
    def test_bt_host_deploy_local_sdk(
        self,
        mock_run,
        _mock_exists,
        _mock_make_writable,
        _mock_sha,
        mock_copy,
        mock_get_project_root,
    ):
        """Test bt-host-deploy command with local SDK."""
        project_root = Path('/tmp/project_root').resolve()
        mock_get_project_root.return_value = project_root
        fuchsia_checkout = Path('/tmp/fuchsia').resolve()
        arm64_far = 'bazel-out/arm64/bin/bt-host.far'
        x64_far = 'bazel-out/x64/bin/bt-host.far'

        # Setup mock responses for subprocess.run
        def side_effect(cmd, *_unused_args, **_unused_kwargs):
            mock_result = MagicMock()
            if 'dump_repo_mapping' in cmd:
                mock_result.stdout = '{"fuchsia_sdk": "fuchsia_sdk_resolved"}'
            elif 'cquery' in cmd:
                if any('pkg.arm64' in arg for arg in cmd):
                    mock_result.stdout = arm64_far
                else:
                    mock_result.stdout = x64_far
            else:
                mock_result.stdout = ''
            mock_result.returncode = 0
            return mock_result

        mock_run.side_effect = side_effect

        args = argparse.Namespace(
            fuchsia_checkout=fuchsia_checkout, local_fuchsia_sdk=True
        )

        result = fuchsia_utils.bt_host_deploy(args)
        self.assertEqual(result, 0)

        override_repo_flag = (
            '--override_repository=fuchsia_sdk_resolved='
            f"{fuchsia_checkout}/out/default/obj/sdk/final_fuchsia_sdk"
        )
        expected_calls = [
            call(
                ['fx', 'build', '//sdk:final_fuchsia_sdk'],
                cwd=fuchsia_checkout,
                check=True,
            ),
            call(
                ['bazelisk', 'mod', 'dump_repo_mapping', ''],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # cquery arm64
            call(
                [
                    'bazelisk',
                    'cquery',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64',
                    '--output=files',
                ],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # build arm64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64',
                ],
                check=True,
                cwd=project_root,
            ),
            # debug symbols arm64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:'
                    'pkg.arm64.debug_symbols',
                ],
                check=True,
                cwd=project_root,
            ),
            # cquery x64
            call(
                [
                    'bazelisk',
                    'cquery',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64',
                    '--output=files',
                ],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # build x64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64',
                ],
                check=True,
                cwd=project_root,
            ),
            # debug symbols x64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '--@fuchsia_sdk//flags:fuchsia_api_level=HEAD',
                    override_repo_flag,
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:'
                    'pkg.x64.debug_symbols',
                ],
                check=True,
                cwd=project_root,
            ),
        ]
        mock_run.assert_has_calls(expected_calls)
        mock_copy.assert_has_calls(
            [
                call(
                    (project_root / arm64_far).resolve(),
                    fuchsia_checkout
                    / 'prebuilt/connectivity/bluetooth/bt-host/arm64/bt-host',
                ),
                call(
                    (project_root / x64_far).resolve(),
                    fuchsia_checkout
                    / 'prebuilt/connectivity/bluetooth/bt-host/x64/bt-host',
                ),
            ]
        )

    @patch(
        'pw_bluetooth_sapphire.fuchsia_utils.get_project_root',
        return_value=Path('/tmp/project_root'),
    )
    @patch('shutil.copy')
    @patch(
        'pw_bluetooth_sapphire.fuchsia_utils.get_sha256',
        return_value='fake_sha',
    )
    @patch('pw_bluetooth_sapphire.fuchsia_utils.make_writable')
    @patch('pathlib.Path.exists', return_value=True)
    @patch('subprocess.run')
    def test_bt_host_deploy_no_local_sdk(
        self,
        mock_run,
        _mock_exists,
        _mock_make_writable,
        _mock_sha,
        mock_copy,
        mock_get_project_root,
    ):
        """Test bt-host-deploy command without local SDK."""
        project_root = Path('/tmp/project_root').resolve()
        mock_get_project_root.return_value = project_root
        fuchsia_checkout = Path('/tmp/fuchsia').resolve()
        arm64_far = 'bazel-out/arm64/bin/bt-host.far'
        x64_far = 'bazel-out/x64/bin/bt-host.far'

        # Setup mock responses for subprocess.run
        def side_effect(cmd, *_unused_args, **_unused_kwargs):
            mock_result = MagicMock()
            if 'dump_repo_mapping' in cmd:
                mock_result.stdout = '{"fuchsia_sdk": "fuchsia_sdk_resolved"}'
            elif 'cquery' in cmd:
                if any('pkg.arm64' in arg for arg in cmd):
                    mock_result.stdout = arm64_far
                else:
                    mock_result.stdout = x64_far
            else:
                mock_result.stdout = ''
            mock_result.returncode = 0
            return mock_result

        mock_run.side_effect = side_effect

        args = argparse.Namespace(
            fuchsia_checkout=fuchsia_checkout, local_fuchsia_sdk=False
        )

        result = fuchsia_utils.bt_host_deploy(args)
        self.assertEqual(result, 0)

        expected_calls = [
            call(
                ['bazelisk', 'mod', 'dump_repo_mapping', ''],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # cquery arm64
            call(
                [
                    'bazelisk',
                    'cquery',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64',
                    '--output=files',
                ],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # build arm64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.arm64',
                ],
                check=True,
                cwd=project_root,
            ),
            # debug symbols arm64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:'
                    'pkg.arm64.debug_symbols',
                ],
                check=True,
                cwd=project_root,
            ),
            # cquery x64
            call(
                [
                    'bazelisk',
                    'cquery',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64',
                    '--output=files',
                ],
                capture_output=True,
                text=True,
                check=True,
                cwd=project_root,
            ),
            # build x64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:pkg.x64',
                ],
                check=True,
                cwd=project_root,
            ),
            # debug symbols x64
            call(
                [
                    'bazelisk',
                    'build',
                    '--config=fuchsia',
                    '//pw_bluetooth_sapphire/fuchsia/bt_host:'
                    'pkg.x64.debug_symbols',
                ],
                check=True,
                cwd=project_root,
            ),
        ]
        mock_run.assert_has_calls(expected_calls)
        mock_copy.assert_has_calls(
            [
                call(
                    (project_root / arm64_far).resolve(),
                    fuchsia_checkout
                    / 'prebuilt/connectivity/bluetooth/bt-host/arm64/bt-host',
                ),
                call(
                    (project_root / x64_far).resolve(),
                    fuchsia_checkout
                    / 'prebuilt/connectivity/bluetooth/bt-host/x64/bt-host',
                ),
            ]
        )

    def test_make_writable_already_writable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = Path(tmpdir) / 'file.txt'
            file_path.write_text('content')
            file_path.chmod(0o755)

            with patch('subprocess.run') as mock_run:
                fuchsia_utils.make_writable(file_path)
                mock_run.assert_not_called()
                self.assertTrue(os.access(file_path, os.W_OK))

    def test_make_writable_nonexistent_file_writable_parent(
        self,
    ):  # pylint: disable=no-self-use
        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = Path(tmpdir) / 'nonexistent.txt'

            with patch('subprocess.run') as mock_run:
                fuchsia_utils.make_writable(file_path)
                mock_run.assert_not_called()

    def test_make_writable_read_only_file_chmod_succeeds(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = Path(tmpdir) / 'file.txt'
            file_path.write_text('content')
            file_path.chmod(0o555)
            self.assertFalse(os.access(file_path, os.W_OK))

            with patch('subprocess.run') as mock_run:
                fuchsia_utils.make_writable(file_path)
                mock_run.assert_not_called()
                self.assertTrue(os.access(file_path, os.W_OK))

    @unittest.skipIf(
        sys.platform == 'win32',
        'Read-only directory permissions not supported on Windows',
    )
    def test_make_writable_read_only_parent_chmod_succeeds(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            parent_dir = Path(tmpdir) / 'sub'
            parent_dir.mkdir()
            file_path = parent_dir / 'file.txt'
            file_path.write_text('content')
            file_path.chmod(0o555)
            parent_dir.chmod(0o555)
            self.assertFalse(os.access(parent_dir, os.W_OK))
            self.assertFalse(os.access(file_path, os.W_OK))

            with patch('subprocess.run') as mock_run:
                fuchsia_utils.make_writable(file_path)
                mock_run.assert_not_called()
                self.assertTrue(os.access(parent_dir, os.W_OK))
                self.assertTrue(os.access(file_path, os.W_OK))

    @unittest.skipIf(
        sys.platform == 'win32',
        'pwd and grp modules not available on Windows',
    )
    def test_make_writable_fallback_to_sudo_accepted(
        self,
    ):  # pylint: disable=no-self-use
        """Test fallback to sudo when user accepts prompt."""
        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = Path(tmpdir) / 'file.txt'
            file_path.write_text('content')

            with (
                patch('os.access', return_value=False),
                patch(
                    'pathlib.Path.chmod',
                    side_effect=OSError('Permission denied'),
                ),
                patch('builtins.input', return_value='y'),
                patch('pwd.getpwuid') as mock_pwd,
                patch('grp.getgrgid') as mock_grp,
                patch('subprocess.run') as mock_run,
            ):
                mock_pwd.return_value.pw_name = 'testuser'
                mock_grp.return_value.gr_name = 'testgroup'

                fuchsia_utils.make_writable(file_path)

                expected_calls = [
                    call(['sudo', 'chmod', '700', str(file_path)], check=True),
                    call(
                        ['sudo', 'chown', 'testuser:testgroup', str(file_path)],
                        check=True,
                    ),
                ]
                mock_run.assert_has_calls(expected_calls)

    @unittest.skipIf(
        sys.platform != 'win32',
        'Windows-specific error test',
    )
    def test_make_writable_fallback_windows_raises(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = Path(tmpdir) / 'file.txt'
            file_path.write_text('content')

            with (
                patch('os.access', return_value=False),
                patch(
                    'pathlib.Path.chmod',
                    side_effect=OSError('Permission denied'),
                ),
            ):
                with self.assertRaises(RuntimeError):
                    fuchsia_utils.make_writable(file_path)


if __name__ == '__main__':
    unittest.main()
