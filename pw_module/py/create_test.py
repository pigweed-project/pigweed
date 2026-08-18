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
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from pw_module.create import (
    _BuildFile,
    _BUILD_FILES,
    _ConfigErrors,
    _GnBuildFile,
    _ModuleConfig,
    _PythonLanguageGenerator,
)


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

    def test_gn_build_file_creation(self):
        """Tests that _GnBuildFile correctly generates a BUILD.gn file."""

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            # Create a minimal context
            mock_ctx = SimpleNamespace(is_upstream=True)

            gn_file = _GnBuildFile(tmp_path, mock_ctx)

            gn_file.add_cc_target(
                _BuildFile.CcTarget(
                    name='pw_async3',
                    headers=[tmp_path / 'public/pw_async3/headers.h'],
                    sources=[tmp_path / 'source.cc'],
                    deps=['//pw_assert'],
                )
            )

            gn_file.add_cc_test(
                _BuildFile.CcTarget(
                    name='pw_async3_test',
                    sources=[tmp_path / 'test.cc'],
                    deps=[':pw_async3'],
                )
            )

            gn_file.add_docs_source('docs.rst')

            with patch('pw_module.create._PW_ROOT', tmp_path):
                gn_file.write()

            generated_file = tmp_path / 'BUILD.gn'
            self.assertTrue(generated_file.exists())

            content = generated_file.read_text()

            self.assertIn('pw_source_set("pw_async3")', content)
            self.assertIn('pw_test("pw_async3_test")', content)
            self.assertIn('pw_doc_group("docs")', content)
            self.assertIn('import("$dir_pw_build/target_types.gni")', content)
            self.assertIn('import("$dir_pw_unit_test/test.gni")', content)
            self.assertIn('import("$dir_pw_docgen/docs.gni")', content)


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
