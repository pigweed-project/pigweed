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
"""Unit tests for pw_py_test_runner."""

# pylint: disable=protected-access

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import warnings

from pw_build import generate_python_package, pw_py_test_runner


class PwPyTestRunnerTest(unittest.TestCase):
    """Tests for pw_py_test_runner warning configuration and execution."""

    @staticmethod
    def _bound_pyproject_loader(root: Path):
        orig_load = pw_py_test_runner._load_pyproject_filterwarnings
        resolved_root = root.resolve()

        def _bounded_load(pyproject_path: Path) -> list[str] | None:
            if not pyproject_path.resolve().is_relative_to(resolved_root):
                return None
            return orig_load(pyproject_path)

        return mock.patch.object(
            pw_py_test_runner,
            '_load_pyproject_filterwarnings',
            side_effect=_bounded_load,
        )

    def test_load_pyproject_filterwarnings_valid(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            pyproject = Path(temp_dir) / 'pyproject.toml'
            pyproject.write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = [\n'
                '    "error::DeprecationWarning:pw_.*",\n'
                '    "ignore::UserWarning",\n'
                ']\n',
                encoding='utf-8',
            )
            filters = pw_py_test_runner._load_pyproject_filterwarnings(
                pyproject
            )
            self.assertEqual(
                filters,
                [
                    'error::DeprecationWarning:pw_.*',
                    'ignore::UserWarning',
                ],
            )

    def test_load_pyproject_filterwarnings_missing_or_empty(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            missing = Path(temp_dir) / 'pyproject.toml'
            self.assertIsNone(
                pw_py_test_runner._load_pyproject_filterwarnings(missing)
            )

            missing.write_text('[build-system]\n', encoding='utf-8')
            self.assertIsNone(
                pw_py_test_runner._load_pyproject_filterwarnings(missing)
            )

    def test_load_pyproject_filterwarnings_invalid_toml_or_type_raises(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            pyproject = Path(temp_dir) / 'pyproject.toml'
            pyproject.write_text('invalid = [unclosed\n', encoding='utf-8')
            with self.assertRaises(Exception):
                pw_py_test_runner._load_pyproject_filterwarnings(pyproject)

            pyproject.write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = "error::DeprecationWarning"\n',
                encoding='utf-8',
            )
            with self.assertRaises(ValueError):
                pw_py_test_runner._load_pyproject_filterwarnings(pyproject)

    def test_find_filterwarnings_from_parent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            pyproject = root / 'pyproject.toml'
            pyproject.write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:custom_.*"]\n',
                encoding='utf-8',
            )
            subdir = root / 'sub' / 'dir'
            subdir.mkdir(parents=True)
            script = subdir / 'my_test.py'
            script.touch()

            filters = pw_py_test_runner._find_filterwarnings(script)
            self.assertEqual(filters, ['error::DeprecationWarning:custom_.*'])

    def test_find_filterwarnings_skips_stub_and_respects_pkg_override(
        self,
    ) -> None:
        """Verifies stub pyproject.toml is skipped and package override wins."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:root_.*"]\n',
                encoding='utf-8',
            )
            pkg = root / 'pw_foo' / 'py'
            pkg.mkdir(parents=True)
            pkg_pyproject = pkg / 'pyproject.toml'
            pkg_pyproject.write_text(
                '[build-system]\nrequires = ["setuptools"]\n',
                encoding='utf-8',
            )
            script = pkg / 'foo_test.py'
            script.touch()

            with self._bound_pyproject_loader(root):
                filters = pw_py_test_runner._find_filterwarnings(script)
            self.assertEqual(filters, ['error::DeprecationWarning:root_.*'])

            pkg_pyproject.write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:pkg_.*"]\n',
                encoding='utf-8',
            )
            with self._bound_pyproject_loader(root):
                filters = pw_py_test_runner._find_filterwarnings(script)
            self.assertEqual(filters, ['error::DeprecationWarning:pkg_.*'])

    def test_find_filterwarnings_from_symlinked_runfiles(self) -> None:
        """Verifies unresolved runfiles symlinks are searched before resolve."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cas_dir = root / 'cas'
            cas_dir.mkdir()
            real_runner = cas_dir / 'blob_runner.py'
            real_runner.touch()
            real_script = cas_dir / 'blob_test.py'
            real_script.touch()

            runfiles_main = root / 'runfiles' / '_main'
            runfiles_main.mkdir(parents=True)
            script_link = runfiles_main / 'pkg' / 'my_test.py'
            script_link.parent.mkdir(parents=True)
            script_link.symlink_to(real_script)
            (runfiles_main / 'pkg' / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:pkg_.*"]\n',
                encoding='utf-8',
            )

            with (
                self._bound_pyproject_loader(root),
                mock.patch.object(Path, 'cwd', return_value=runfiles_main),
                mock.patch.object(
                    pw_py_test_runner, '__file__', str(real_runner)
                ),
            ):
                filters = pw_py_test_runner._find_filterwarnings(script_link)
            self.assertEqual(filters, ['error::DeprecationWarning:pkg_.*'])

    def test_find_filterwarnings_ignores_runner_dir(self) -> None:
        """Verifies runner directory pyproject.toml does not leak to tests."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            script = root / 'my_test.py'
            script.touch()
            runner_root = root / 'runner_repo'
            runner_root.mkdir()
            runner_pyproject = runner_root / 'pyproject.toml'
            runner_pyproject.write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:runner_.*"]\n',
                encoding='utf-8',
            )
            fake_runner = runner_root / 'pw_build' / 'pw_py_test_runner.py'
            fake_runner.parent.mkdir(parents=True)
            fake_runner.touch()

            with (
                self._bound_pyproject_loader(root),
                mock.patch.object(Path, 'cwd', return_value=root),
                mock.patch.object(
                    pw_py_test_runner, '__file__', str(fake_runner)
                ),
            ):
                filters = pw_py_test_runner._find_filterwarnings(script)
            self.assertEqual(filters, [])

    def test_find_filterwarnings_fallback_empty(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            script = root / 'my_test.py'
            script.touch()
            fake_runner = root / 'pw_py_test_runner.py'
            fake_runner.touch()
            with (
                self._bound_pyproject_loader(root),
                mock.patch.object(Path, 'cwd', return_value=root),
                mock.patch.object(
                    pw_py_test_runner, '__file__', str(fake_runner)
                ),
            ):
                filters = pw_py_test_runner._find_filterwarnings(script)
            self.assertEqual(filters, [])

    def test_fallback_empty_when_tmpdir_under_outer_pyproject(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            outer_path = Path(outer)
            (outer_path / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:leaked_.*"]\n',
                encoding='utf-8',
            )
            inner_tmp = outer_path / 'tmp'
            inner_tmp.mkdir()
            with mock.patch.object(tempfile, 'tempdir', str(inner_tmp)):
                self.test_find_filterwarnings_fallback_empty()

    def test_apply_warning_filter_pw_vs_third_party(self) -> None:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter('default')
            pw_py_test_runner._apply_warning_filter(
                'error::DeprecationWarning:pw_.*'
            )

            # DeprecationWarning from a pw_* module should raise an exception.
            with self.assertRaises(DeprecationWarning):
                warnings.warn_explicit(
                    'deprecated pigweed api',
                    DeprecationWarning,
                    'pw_foo/bar.py',
                    10,
                    module='pw_foo.bar',
                )

            # DeprecationWarning from a third-party module should not raise.
            warnings.warn_explicit(
                'deprecated third-party api',
                DeprecationWarning,
                'other_pkg/bar.py',
                10,
                module='other_pkg.bar',
            )
            self.assertEqual(len(caught), 1)

    def test_apply_warning_filter_precedence_and_five_fields(self) -> None:
        """Verifies later filters override earlier ones and parse 5 fields."""
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter('default')
            pw_py_test_runner._apply_warning_filter(
                'error::DeprecationWarning:pw_.*'
            )
            pw_py_test_runner._apply_warning_filter(
                'ignore:legacy api:DeprecationWarning:pw_foo.*:42'
            )

            # Matching message, module, and lineno 42 is ignored.
            warnings.warn_explicit(
                'legacy api used here',
                DeprecationWarning,
                'pw_foo/bar.py',
                42,
                module='pw_foo.bar',
            )
            self.assertEqual(len(caught), 0)

            # Different line number still raises DeprecationWarning.
            with self.assertRaises(DeprecationWarning):
                warnings.warn_explicit(
                    'legacy api used here',
                    DeprecationWarning,
                    'pw_foo/bar.py',
                    99,
                    module='pw_foo.bar',
                )

    def test_invalid_filter_spec_raises(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = [\n'
                '    "invalid_action::DeprecationWarning:pw_.*",\n'
                ']\n',
                encoding='utf-8',
            )
            script = root / 'my_test.py'
            script.touch()
            with warnings.catch_warnings(record=True):
                with self.assertRaises(Exception):
                    pw_py_test_runner._configure_test_warnings(script)

                with self.assertRaises(ValueError):
                    pw_py_test_runner._apply_warning_filter('a:b:c:d:e:f')

    def test_configure_test_warnings_does_not_corrupt_warnoptions(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:pw_.*"]\n',
                encoding='utf-8',
            )
            script = root / 'my_test.py'
            script.touch()
            orig_warnoptions = list(sys.warnoptions)
            try:
                sys.warnoptions.clear()
                with warnings.catch_warnings(record=True):
                    pw_py_test_runner._configure_test_warnings(script)
                    self.assertTrue(sys.warnoptions)
                    for opt in sys.warnoptions:
                        self.assertNotIn('pw_.*', opt)
            finally:
                sys.warnoptions[:] = orig_warnoptions

    def test_main_missing_script_raises_runtime_error(self) -> None:
        with (
            mock.patch.dict(os.environ, {}, clear=False),
            mock.patch.object(sys, 'argv', ['pw_py_test_runner.py']),
        ):
            os.environ.pop('PW_PY_TEST_MAIN', None)
            with self.assertRaisesRegex(
                RuntimeError,
                r'PW_PY_TEST_MAIN.*command-line argument',
            ):
                pw_py_test_runner.main()

    def test_main_sets_absolute_file_and_sys_path_across_chdir(self) -> None:
        """Verifies main() sets an absolute __file__ and cleans sys.path."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sub = root / 'sub'
            sub.mkdir()
            other = root / 'other'
            other.mkdir()
            (sub / 'helper_mod.py').write_text('VAL = 42\n', encoding='utf-8')
            runner_dir = str(Path(pw_py_test_runner.__file__).resolve().parent)
            script = sub / 'chdir_test.py'
            script.write_text(
                'import os, pathlib, sys\n'
                f'assert {runner_dir!r} not in sys.path, sys.path\n'
                f'os.chdir({str(other)!r})\n'
                'import helper_mod\n'
                'assert pathlib.Path(__file__).is_absolute(), __file__\n'
                'assert pathlib.Path(__file__).exists(), __file__\n'
                'assert helper_mod.VAL == 42\n',
                encoding='utf-8',
            )
            orig_cwd = Path.cwd()
            orig_sys_path = list(sys.path)
            orig_warnoptions = list(sys.warnoptions)
            try:
                os.chdir(root)
                sys.path.insert(0, runner_dir)
                with (
                    mock.patch.dict(
                        os.environ, {'PW_PY_TEST_MAIN': 'sub/chdir_test.py'}
                    ),
                    mock.patch.object(sys, 'argv', ['pw_py_test_runner.py']),
                    warnings.catch_warnings(record=True),
                ):
                    pw_py_test_runner.main()
                    self.assertNotIn(runner_dir, sys.path)
            finally:
                os.chdir(orig_cwd)
                sys.path[:] = orig_sys_path
                sys.modules.pop('helper_mod', None)
                sys.warnoptions[:] = orig_warnoptions

    def test_main_runs_unittest_and_enforces_deprecation_error(self) -> None:
        """Verifies main() fails when a unittest emits a DeprecationWarning."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / 'pyproject.toml').write_text(
                '[tool.pytest.ini_options]\n'
                'filterwarnings = ["error::DeprecationWarning:__main__"]\n',
                encoding='utf-8',
            )
            script = root / 'sample_unittest.py'
            script.write_text(
                'import unittest, warnings\n'
                'class SampleTest(unittest.TestCase):\n'
                '    def test_deprecated(self) -> None:\n'
                '        warnings.warn("old", DeprecationWarning)\n'
                'if __name__ == "__main__":\n'
                '    unittest.main()\n',
                encoding='utf-8',
            )
            orig_sys_path = list(sys.path)
            orig_warnoptions = list(sys.warnoptions)
            try:
                with (
                    mock.patch.dict(
                        os.environ, {'PW_PY_TEST_MAIN': str(script)}
                    ),
                    mock.patch.object(sys, 'argv', ['pw_py_test_runner.py']),
                    warnings.catch_warnings(record=True),
                ):
                    with self.assertRaises(SystemExit) as ctx:
                        pw_py_test_runner.main()
                    self.assertTrue(ctx.exception.code)
            finally:
                sys.path[:] = orig_sys_path
                sys.warnoptions[:] = orig_warnoptions

    def test_main_cli_argument_and_flag_forwarding(self) -> None:
        """Verifies main() accepts script via sys.argv and forwards flags."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            script = root / 'cli_arg_test.py'
            script.write_text(
                'import pathlib, sys\n'
                'assert pathlib.Path(sys.argv[0]).is_absolute(), sys.argv\n'
                'assert sys.argv[1:] == ["--custom-flag", "123"], sys.argv\n',
                encoding='utf-8',
            )
            orig_cwd = Path.cwd()
            orig_sys_path = list(sys.path)
            orig_warnoptions = list(sys.warnoptions)
            try:
                os.chdir(root)
                with (
                    mock.patch.dict(os.environ, {}, clear=False),
                    mock.patch.object(
                        sys,
                        'argv',
                        [
                            'pw_py_test_runner.py',
                            'cli_arg_test.py',
                            '--custom-flag',
                            '123',
                        ],
                    ),
                    warnings.catch_warnings(record=True),
                ):
                    os.environ.pop('PW_PY_TEST_MAIN', None)
                    pw_py_test_runner.main()
                    self.assertEqual(
                        sys.argv,
                        [str(script.resolve()), '--custom-flag', '123'],
                    )
            finally:
                os.chdir(orig_cwd)
                sys.path[:] = orig_sys_path
                sys.warnoptions[:] = orig_warnoptions

    def test_generate_pyproject_toml_scopes_filterwarnings(self) -> None:
        """Verifies generate_python_package emits module-scoped filters."""
        content = generate_python_package._generate_pyproject_toml(
            {'pw_rpc': set(), 'pw_rpc.codegen': set()},
            {'metadata': {'name': 'pw_rpc'}},
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            pyproject = Path(temp_dir) / 'pyproject.toml'
            pyproject.write_text(content, encoding='utf-8')
            filters = pw_py_test_runner._load_pyproject_filterwarnings(
                pyproject
            )
            self.assertEqual(
                filters,
                [
                    'error::DeprecationWarning:pw_rpc.*',
                    'error::DeprecationWarning:__main__',
                ],
            )


if __name__ == '__main__':
    unittest.main()
