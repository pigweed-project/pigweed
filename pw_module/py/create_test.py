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
"""Tests for pw_module create."""

from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from pw_module.create import (
    _add_to_pigweed_modules_file,
    _BazelBuildFile,
    _BuildFile,
    _BUILD_FILES,
    _CmakeBuildFile,
    _ConfigErrors,
    _GnBuildFile,
    _gn_tool,
    _ModuleConfig,
    _ModuleName,
    _OutputFile,
    _PythonLanguageGenerator,
)


def _mock_context(is_upstream: bool = True) -> SimpleNamespace:
    return SimpleNamespace(
        is_upstream=is_upstream,
        name=SimpleNamespace(full='pw_async3', path='pw_async3'),
    )


def _add_cc_targets(build_file: _BuildFile, module_dir: Path) -> None:
    build_file.add_cc_target(
        _BuildFile.CcTarget(
            name='pw_async3',
            headers=[module_dir / 'public/pw_async3/headers.h'],
            sources=[module_dir / 'source.cc'],
            deps=['//pw_assert'],
        )
    )
    build_file.add_cc_test(
        _BuildFile.CcTarget(
            name='pw_async3_test',
            sources=[module_dir / 'test.cc'],
            deps=[':pw_async3'],
        )
    )


def _write(build_file: _BuildFile, project_root: Path) -> str:
    with patch('pw_module.create._project_root', return_value=project_root):
        build_file.write()
    return build_file.path.read_text()


class TestReporting(unittest.TestCase):
    """Tests reporting of written files."""

    def test_write_without_pw_root(self):
        """Files are reported relative to the project root, not PW_ROOT."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            with (
                patch('pw_module.create._PW_ROOT', None),
                patch('pw_module.create._project_root', return_value=tmp_path),
            ):
                _OutputFile(tmp_path / 'docs.rst').write('hi')

            self.assertEqual((tmp_path / 'docs.rst').read_text(), 'hi\n')


class TestModuleConfig(unittest.TestCase):
    """Tests config parsing."""

    @patch('pw_env_setup.config_file.load')
    def test_load_valid_config(self, mock_load):
        """Test loading a valid config."""
        mock_load.return_value = {
            'pw': {
                'pw_module': {
                    'default_build_systems': ['bazel'],
                    'default_languages': ['cc'],
                }
            }
        }
        config = _ModuleConfig.load()
        self.assertEqual(config.default_build_systems, ['bazel'])
        self.assertEqual(config.default_languages, ['cc'])

    @patch('pw_env_setup.config_file.load')
    def test_load_empty_config(self, mock_load):
        """Test loading an empty config."""
        mock_load.return_value = {}
        config = _ModuleConfig.load()
        self.assertEqual(
            config.default_build_systems, list(_BUILD_FILES.keys())
        )
        self.assertEqual(config.default_languages, [])

    @patch('pw_env_setup.config_file.load')
    def test_load_invalid_build_system(self, mock_load):
        mock_load.return_value = {
            'pw': {
                'pw_module': {
                    'default_build_systems': ['ninja'],
                }
            }
        }
        result = _ModuleConfig.load()
        self.assertIsInstance(result, _ConfigErrors)
        self.assertEqual(len(result), 1)
        self.assertIn('Invalid build systems', result[0])
        self.assertIn('ninja', result[0])

    @patch('pw_env_setup.config_file.load')
    def test_load_invalid_language(self, mock_load):
        mock_load.return_value = {
            'pw': {
                'pw_module': {
                    'default_languages': ['cobol'],
                }
            }
        }
        result = _ModuleConfig.load()
        self.assertIsInstance(result, _ConfigErrors)
        self.assertEqual(len(result), 1)
        self.assertIn('Invalid languages', result[0])
        self.assertIn('cobol', result[0])

    @patch('pw_env_setup.config_file.load')
    def test_load_multiple_invalid(self, mock_load):
        mock_load.return_value = {
            'pw': {
                'pw_module': {
                    'default_build_systems': ['bazel', 'ninja'],
                    'default_languages': ['cobol', 'cc'],
                }
            }
        }
        result = _ModuleConfig.load()
        self.assertIsInstance(result, _ConfigErrors)
        self.assertEqual(len(result), 2)
        self.assertTrue(
            any('Invalid build systems' in e and 'ninja' in e for e in result)
        )
        self.assertTrue(
            any('Invalid languages' in e and 'cobol' in e for e in result)
        )


class TestGnModuleCreation(unittest.TestCase):
    """Tests GN module creation."""

    def assert_formatted_gn(self, path: Path) -> None:
        """Checks that a file is valid GN that gn format leaves unchanged."""
        # Format through stdin to ignore platform-specific line endings.
        content = path.read_text()
        result = subprocess.run(
            [str(_gn_tool()), 'format', '--stdin'],
            input=content.encode(),
            capture_output=True,
        )
        self.assertEqual(
            result.returncode,
            0,
            f'gn format failed for:\n{content}\n{result.stderr.decode()}',
        )
        self.assertEqual(result.stdout.decode().replace('\r\n', '\n'), content)

    def test_gn_build_file_creation(self):
        """Tests that _GnBuildFile correctly generates a BUILD.gn file."""

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            gn_file = _GnBuildFile(tmp_path, _mock_context())
            _add_cc_targets(gn_file, tmp_path)
            gn_file.add_docs_source('docs.rst')

            content = _write(gn_file, tmp_path)

            self.assertIn('pw_source_set("pw_async3")', content)
            self.assertIn('pw_test("pw_async3_test")', content)
            self.assertIn('pw_doc_group("docs")', content)
            self.assertIn('import("$dir_pw_build/target_types.gni")', content)
            self.assertIn('import("$dir_pw_unit_test/test.gni")', content)
            self.assertIn('import("$dir_pw_docgen/docs.gni")', content)
            self.assertIn('public = [ "public/pw_async3/headers.h" ]', content)
            self.assertIn('tests = [ ":pw_async3_test" ]', content)
            self.assert_formatted_gn(gn_file.path)

    def test_gn_build_file_without_cc(self):
        """Tests a BUILD.gn file for an upstream module with no C++."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            gn_file = _GnBuildFile(tmp_path, _mock_context())
            gn_file.add_docs_source('docs.rst')

            content = _write(gn_file, tmp_path)

            self.assertIn('tests = []', content)
            self.assertNotIn('pw_source_set', content)
            self.assert_formatted_gn(gn_file.path)

    def test_gn_build_file_downstream(self):
        """Tests a BUILD.gn file for a downstream module."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            gn_file = _GnBuildFile(tmp_path, _mock_context(is_upstream=False))
            _add_cc_targets(gn_file, tmp_path)
            gn_file.add_docs_source('docs.rst')

            content = _write(gn_file, tmp_path)

            self.assertNotIn('Copyright', content)
            self.assert_formatted_gn(gn_file.path)

    def test_add_to_pigweed_modules_file(self):
        """Tests that the generated modules list is updated and formatted."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'PIGWEED_MODULES').write_text('pw_a\npw_c\n')
            (tmp_path / 'pw_build').mkdir()

            module_name = _ModuleName.parse('pw_b')
            assert module_name is not None
            with patch('pw_module.create._project_root', return_value=tmp_path):
                _add_to_pigweed_modules_file(tmp_path, module_name, _gn_tool())

            self.assertEqual(
                (tmp_path / 'PIGWEED_MODULES').read_text(), 'pw_a\npw_b\npw_c\n'
            )
            gni_file = tmp_path / 'pw_build/generated_pigweed_modules_lists.gni'
            self.assertIn(
                '  dir_pw_b = get_path_info("../pw_b", "abspath")',
                gni_file.read_text(),
            )
            self.assert_formatted_gn(gni_file)

    def test_add_to_pigweed_modules_file_failure(self):
        """Tests that a failed modules list update is reported as an error."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'PIGWEED_MODULES').write_text('pw_a\n')
            (tmp_path / 'pw_build').mkdir()

            module_name = _ModuleName.parse('pw_b')
            assert module_name is not None
            with (
                patch('pw_module.create._project_root', return_value=tmp_path),
                patch(
                    'pw_module.create.generate_modules_lists.main',
                    return_value=1,
                ),
                patch('pw_module.create._report_edited_file') as report_edit,
                self.assertLogs('pw_module.create', level='ERROR') as logs,
            ):
                _add_to_pigweed_modules_file(tmp_path, module_name, Path('gn'))

            report_edit.assert_called_once_with(tmp_path / 'PIGWEED_MODULES')
            gni_path = Path('pw_build', 'generated_pigweed_modules_lists.gni')
            self.assertIn(f'Failed to update {gni_path}', logs.output[0])


class TestBazelModuleCreation(unittest.TestCase):
    """Tests Bazel module creation."""

    def test_bazel_build_file_creation(self):
        """Tests that _BazelBuildFile correctly generates a BUILD.bazel file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            bazel_file = _BazelBuildFile(tmp_path, _mock_context())
            _add_cc_targets(bazel_file, tmp_path)

            content = _write(bazel_file, tmp_path)

            self.assertIn(
                'load("@rules_cc//cc:cc_library.bzl", "cc_library")', content
            )
            self.assertIn(
                'load("//pw_unit_test:pw_cc_test.bzl", "pw_cc_test")', content
            )


class TestCmakeModuleCreation(unittest.TestCase):
    """Tests CMake module creation."""

    def test_cmake_build_file_creation(self):
        """Tests that _CmakeBuildFile generates a CMakeLists.txt file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            cmake_file = _CmakeBuildFile(tmp_path, _mock_context())
            _add_cc_targets(cmake_file, tmp_path)

            content = _write(cmake_file, tmp_path)

            self.assertIn('pw_add_library(pw_async3 STATIC', content)
            self.assertNotIn('pw_add_module_library', content)
            self.assertIn('pw_add_test(pw_async3.pw_async3_test', content)


class TestPythonModuleCreation(unittest.TestCase):
    """Tests Python module creation."""

    def test_python_source_files_creation_upstream(self):
        """Tests that correct source files are created with license headers."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            mock_name = SimpleNamespace(
                full='pw_foo_py', main='foo_py', prefix='pw'
            )
            mock_ctx = SimpleNamespace(
                name=mock_name,
                dir=tmp_path,
                is_upstream=True,
                build_systems=['bazel', 'gn'],
            )
            generator = _PythonLanguageGenerator(mock_ctx)

            with (
                patch('pw_module.create._prompt_overwrite', return_value=True),
                patch('pw_module.create._report_write_file'),
            ):
                generator.create_source_files()

            self.assertTrue(
                (tmp_path / 'py' / 'pw_foo_py' / '__init__.py').exists()
            )
            self.assertTrue((tmp_path / 'py' / 'pyproject.toml').exists())
            self.assertTrue((tmp_path / 'py' / 'setup.cfg').exists())
            self.assertTrue((tmp_path / 'py' / 'foo_py_test.py').exists())
            self.assertTrue((tmp_path / 'py' / 'BUILD.gn').exists())
            self.assertTrue((tmp_path / 'py' / 'BUILD.bazel').exists())

            init_content = (
                tmp_path / 'py' / 'pw_foo_py' / '__init__.py'
            ).read_text()
            self.assertIn('Copyright', init_content)
            self.assertIn('The Pigweed Authors', init_content)

            pyproject_content = (tmp_path / 'py' / 'pyproject.toml').read_text()
            self.assertIn('[build-system]', pyproject_content)

            setup_content = (tmp_path / 'py' / 'setup.cfg').read_text()
            self.assertIn('name = pw_foo_py', setup_content)

            test_content = (tmp_path / 'py' / 'foo_py_test.py').read_text()
            self.assertIn('import unittest', test_content)
            self.assertIn('class TestStub', test_content)

            bazel_content = (tmp_path / 'py' / 'BUILD.bazel').read_text()
            self.assertIn('name = "pw_foo_py"', bazel_content)
            self.assertIn('name = "foo_py_test"', bazel_content)

    def test_python_source_files_creation_downstream(self):
        """Tests that correct source files are created without headers."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            mock_name = SimpleNamespace(full='foo_py', main='foo_py', prefix='')
            mock_ctx = SimpleNamespace(
                name=mock_name,
                dir=tmp_path,
                is_upstream=False,
                build_systems=['bazel'],
            )
            generator = _PythonLanguageGenerator(mock_ctx)

            with (
                patch('pw_module.create._prompt_overwrite', return_value=True),
                patch('pw_module.create._report_write_file'),
            ):
                generator.create_source_files()

            init_content = (
                tmp_path / 'py' / 'foo_py' / '__init__.py'
            ).read_text()
            self.assertNotIn('Copyright', init_content)

            setup_content = (tmp_path / 'py' / 'setup.cfg').read_text()
            self.assertIn('name = foo_py', setup_content)

    def test_python_source_files_creation_only_bazel(self):
        """Tests that only Bazel files are created when specified."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            mock_name = SimpleNamespace(
                full='pw_foo_py', main='foo_py', prefix='pw'
            )
            mock_ctx = SimpleNamespace(
                name=mock_name,
                dir=tmp_path,
                is_upstream=True,
                build_systems=['bazel'],
            )
            generator = _PythonLanguageGenerator(mock_ctx)

            with (
                patch('pw_module.create._prompt_overwrite', return_value=True),
                patch('pw_module.create._report_write_file'),
            ):
                generator.create_source_files()

            self.assertFalse((tmp_path / 'py' / 'BUILD.gn').exists())
            self.assertTrue((tmp_path / 'py' / 'BUILD.bazel').exists())


if __name__ == '__main__':
    unittest.main()
