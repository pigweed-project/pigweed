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
    PromptChoice,
    _add_to_docs_build,
    _add_to_module_metadata,
    _add_to_pigweed_modules_file,
    _add_to_root_cmakelists,
    _BazelBuildFile,
    _BuildFile,
    _BUILD_FILES,
    _CcLanguageGenerator,
    _check_module_name,
    _CmakeBuildFile,
    _ConfigErrors,
    _create_module,
    _GnBuildFile,
    _gn_tool,
    _ModuleConfig,
    _ModuleContext,
    _ModuleName,
    _OutputFile,
    _prompt_overwrite,
    _PythonLanguageGenerator,
    _read_root_owners,
)
import pw_module.check


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


class TestModuleName(unittest.TestCase):
    """Tests module name parsing and validation."""

    def test_valid_module_names(self):
        """Tests parsing valid top-level and nested module names."""
        name = _ModuleName.parse('pw_foo')
        self.assertIsNotNone(name)
        assert name is not None
        self.assertEqual(name.prefix, 'pw')
        self.assertEqual(name.main, 'foo')
        self.assertEqual(name.full, 'pw_foo')
        self.assertEqual(name.path, 'pw_foo')
        self.assertEqual(name.default_namespace, 'pw::foo')
        self.assertEqual(name.header_line, '======')

        nested = _ModuleName.parse('dir/sub/custom_bar_baz2')
        self.assertIsNotNone(nested)
        assert nested is not None
        self.assertEqual(nested.prefix, 'custom')
        self.assertEqual(nested.main, 'bar_baz2')
        self.assertEqual(nested.full, 'custom_bar_baz2')
        self.assertEqual(nested.path, 'dir/sub/custom_bar_baz2')
        self.assertEqual(nested.default_namespace, 'custom::bar_baz2')

    def test_invalid_module_names(self):
        for bad in (
            'foo',
            'p_foo',
            'pw__foo',
            'pw_foo_',
            '_pw_foo',
            'pw_foo-bar',
            '12_foo',
        ):
            with self.subTest(bad=bad):
                self.assertIsNone(_ModuleName.parse(bad))

    def test_check_module_name_upstream_vs_downstream(self):
        self.assertIsNotNone(_check_module_name('pw_foo', is_upstream=True))
        self.assertIsNone(_check_module_name('app_foo', is_upstream=True))
        self.assertIsNotNone(_check_module_name('app_foo', is_upstream=False))


class TestCcModuleCreation(unittest.TestCase):
    """Tests C++ module source and header generation."""

    def test_cc_source_files_creation_upstream(self):
        """Tests C++ file generation for an upstream module."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            module_name = _ModuleName.parse('pw_foo_bar')
            assert module_name is not None
            ctx = _ModuleContext(
                name=module_name,
                dir=tmp_path,
                root_build_files=[],
                sub_build_files=[],
                build_systems=['bazel', 'gn', 'cmake'],
                is_upstream=True,
            )
            generator = _CcLanguageGenerator(ctx)

            with (
                patch('pw_module.create._prompt_overwrite', return_value=True),
                patch('pw_module.create._report_write_file'),
            ):
                generator.create_source_files()

            header = tmp_path / 'public' / 'pw_foo_bar' / 'foo_bar.h'
            source = tmp_path / 'foo_bar.cc'
            test = tmp_path / 'foo_bar_test.cc'

            self.assertTrue(header.exists())
            self.assertTrue(source.exists())
            self.assertTrue(test.exists())

            header_text = header.read_text()
            self.assertIn('// Copyright', header_text)
            self.assertIn('#pragma once', header_text)
            self.assertIn('namespace pw::foo_bar {', header_text)

            source_text = source.read_text()
            self.assertIn('#include "pw_foo_bar/foo_bar.h"', source_text)
            self.assertIn('int magic = 42;', source_text)

            test_text = test.read_text()
            self.assertIn('#include "pw_foo_bar/foo_bar.h"', test_text)
            self.assertIn('TEST(FooBar, GeneratesCorrectly)', test_text)

    def test_cc_source_files_creation_downstream(self):
        """Tests C++ file generation for a downstream module."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            module_name = _ModuleName.parse('app_widget')
            assert module_name is not None
            ctx = _ModuleContext(
                name=module_name,
                dir=tmp_path,
                root_build_files=[],
                sub_build_files=[],
                build_systems=['bazel'],
                is_upstream=False,
            )
            generator = _CcLanguageGenerator(ctx)

            with (
                patch('pw_module.create._prompt_overwrite', return_value=True),
                patch('pw_module.create._report_write_file'),
            ):
                generator.create_source_files()

            header = tmp_path / 'public' / 'app_widget' / 'widget.h'
            self.assertNotIn('Copyright', header.read_text())
            self.assertIn('namespace app::widget {', header.read_text())


class TestUpstreamIntegration(unittest.TestCase):
    """Tests upstream registration helpers."""

    def test_add_to_module_metadata(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'pw_foo').mkdir()
            module_name = _ModuleName.parse('pw_foo')
            assert module_name is not None

            with patch('pw_module.create._project_root', return_value=tmp_path):
                created = _add_to_module_metadata(
                    tmp_path, module_name, ['cc', 'py']
                )
                self.assertTrue(created)
                created_again = _add_to_module_metadata(
                    tmp_path, module_name, ['cc', 'py']
                )
                self.assertFalse(created_again)

            toml_text = (tmp_path / 'pw_foo' / 'module.toml').read_text()
            self.assertIn('status = "experimental"', toml_text)
            self.assertIn('languages = ["C++", "Python"]', toml_text)

    def test_add_to_docs_build(self):
        """Tests inserting a module docs target into docs/sphinx/BUILD.bazel."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            sphinx_dir = tmp_path / 'docs' / 'sphinx'
            sphinx_dir.mkdir(parents=True)
            build_file = sphinx_dir / 'BUILD.bazel'
            build_file.write_text(
                'sphinx_docs_library(\n'
                '    srcs = [\n'
                '        # start (sentinel for pw module create)\n'
                '        "//pw_alpha:docs",\n'
                '        "//pw_zeta:docs",\n'
                '        # end (sentinel for pw module create)\n'
                '    ],\n'
                ')\n'
            )

            module_name = _ModuleName.parse('pw_beta')
            assert module_name is not None
            with patch('pw_module.create._project_root', return_value=tmp_path):
                _add_to_docs_build(tmp_path, module_name)
                # Calling a second time is a no-op.
                _add_to_docs_build(tmp_path, module_name)

            self.assertEqual(
                build_file.read_text(),
                'sphinx_docs_library(\n'
                '    srcs = [\n'
                '        # start (sentinel for pw module create)\n'
                '        "//pw_alpha:docs",\n'
                '        "//pw_beta:docs",\n'
                '        "//pw_zeta:docs",\n'
                '        # end (sentinel for pw module create)\n'
                '    ],\n'
                ')\n',
            )

    def test_add_to_root_cmakelists(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            cmake_file = tmp_path / 'CMakeLists.txt'
            cmake_file.write_text(
                '# Header\n'
                'add_subdirectory(pw_alpha EXCLUDE_FROM_ALL)\n'
                'add_subdirectory(pw_zeta EXCLUDE_FROM_ALL)\n'
            )

            module_name = _ModuleName.parse('pw_beta')
            assert module_name is not None
            with patch('pw_module.create._project_root', return_value=tmp_path):
                _add_to_root_cmakelists(tmp_path, module_name)
                _add_to_root_cmakelists(tmp_path, module_name)

            self.assertEqual(
                cmake_file.read_text(),
                '# Header\n'
                'add_subdirectory(pw_alpha EXCLUDE_FROM_ALL)\n'
                'add_subdirectory(pw_beta EXCLUDE_FROM_ALL)\n'
                'add_subdirectory(pw_zeta EXCLUDE_FROM_ALL)\n',
            )

    def test_read_root_owners(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'OWNERS').write_text(
                '# Top-level comment\n'
                'alice@example.com  # inline comment\n'
                '\n'
                'bob@example.com\n'
            )
            self.assertEqual(
                list(_read_root_owners(tmp_path)),
                ['alice@example.com', 'bob@example.com'],
            )


class TestPromptOverwrite(unittest.TestCase):
    """Tests file overwrite prompting and diff display."""

    def test_prompt_overwrite_flows(self):
        """Tests new, identical, diff+overwrite, and skip overwrite flows."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            target = tmp_path / 'file.txt'

            with patch('pw_module.create._project_root', return_value=tmp_path):
                # Non-existent file -> True without prompting.
                self.assertTrue(_prompt_overwrite(target, 'hello\n'))

                # Identical file -> False without prompting.
                target.write_text('hello\n', encoding='utf-8')
                self.assertFalse(_prompt_overwrite(target, 'hello\n'))

                # Different file -> user chooses DIFF then YES.
                with (
                    patch(
                        'pw_module.create._prompt_user',
                        side_effect=[PromptChoice.DIFF, PromptChoice.YES],
                    ) as mock_prompt,
                    patch('pw_module.create._print_diff') as mock_diff,
                ):
                    self.assertTrue(_prompt_overwrite(target, 'updated\n'))
                    self.assertEqual(mock_prompt.call_count, 2)
                    mock_diff.assert_called_once()

                # Different file -> user chooses NO.
                with patch(
                    'pw_module.create._prompt_user',
                    return_value=PromptChoice.NO,
                ):
                    self.assertFalse(_prompt_overwrite(target, 'updated\n'))


class TestEndToEndCreateModule(unittest.TestCase):
    """End-to-end integration tests for _create_module."""

    def test_end_to_end_upstream_module_creation_and_check(self):
        """Tests creating an upstream module and verifying it passes check."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'PIGWEED_MODULES').write_text('pw_alpha\npw_zeta\n')
            (tmp_path / 'pw_build').mkdir()
            (tmp_path / 'OWNERS').write_text('root@google.com\n')
            (tmp_path / 'CMakeLists.txt').write_text(
                'add_subdirectory(pw_alpha EXCLUDE_FROM_ALL)\n'
                'add_subdirectory(pw_zeta EXCLUDE_FROM_ALL)\n'
            )
            sphinx_dir = tmp_path / 'docs' / 'sphinx'
            sphinx_dir.mkdir(parents=True)
            (sphinx_dir / 'BUILD.bazel').write_text(
                '    srcs = [\n'
                '        # start (sentinel for pw module create)\n'
                '        "//pw_alpha:docs",\n'
                '        "//pw_zeta:docs",\n'
                '        # end (sentinel for pw module create)\n'
                '    ],\n'
            )

            with (
                patch('pw_module.create._project_root', return_value=tmp_path),
                patch('pw_module.create._is_upstream', return_value=True),
                patch('pw_env_setup.config_file.load', return_value={}),
            ):
                _create_module(
                    module='pw_beta',
                    languages=['cc', 'py'],
                    build_systems=['bazel', 'cmake', 'gn'],
                    owners=['second@google.com', 'root@google.com'],
                )

            mod_dir = tmp_path / 'pw_beta'
            self.assertTrue((mod_dir / 'docs.rst').is_file())
            self.assertTrue((mod_dir / 'module.toml').is_file())
            self.assertTrue((mod_dir / 'OWNERS').is_file())
            self.assertEqual(
                (mod_dir / 'OWNERS').read_text(),
                'root@google.com\nsecond@google.com\n',
            )
            self.assertTrue((mod_dir / 'BUILD.bazel').is_file())
            self.assertTrue((mod_dir / 'BUILD.gn').is_file())
            self.assertTrue((mod_dir / 'CMakeLists.txt').is_file())
            self.assertTrue((mod_dir / 'public/pw_beta/beta.h').is_file())
            self.assertTrue((mod_dir / 'beta.cc').is_file())
            self.assertTrue((mod_dir / 'beta_test.cc').is_file())
            self.assertTrue((mod_dir / 'py/pw_beta/__init__.py').is_file())
            self.assertTrue((mod_dir / 'py/beta_test.py').is_file())
            self.assertTrue((mod_dir / 'py/BUILD.bazel').is_file())
            self.assertTrue((mod_dir / 'py/BUILD.gn').is_file())

            # PWCK001 checks for setup.py when Python files exist; add stub to
            # verify PWCK002-PWCK005 pass on a newly created module.
            (mod_dir / 'py/setup.py').write_text('')
            self.assertTrue(pw_module.check.check_module(str(mod_dir)))

    def test_owners_validation_errors(self):
        """Tests --owners validation for count, email format, and root OWNER."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            (tmp_path / 'OWNERS').write_text('root@google.com\n')

            with (
                patch('pw_module.create._project_root', return_value=tmp_path),
                patch('pw_module.create._is_upstream', return_value=True),
                patch('pw_env_setup.config_file.load', return_value={}),
            ):
                # Fewer than 2 owners -> sys.exit(1)
                with self.assertRaises(SystemExit):
                    _create_module(
                        module='pw_beta',
                        languages=['cc'],
                        build_systems=['bazel'],
                        owners=['root@google.com'],
                    )
                # Invalid email -> sys.exit(1)
                with self.assertRaises(SystemExit):
                    _create_module(
                        module='pw_beta',
                        languages=['cc'],
                        build_systems=['bazel'],
                        owners=['root@google.com', 'not-an-email'],
                    )
                # No root owner -> sys.exit(1)
                with self.assertRaises(SystemExit):
                    _create_module(
                        module='pw_beta',
                        languages=['cc'],
                        build_systems=['bazel'],
                        owners=['a@google.com', 'b@google.com'],
                    )


if __name__ == '__main__':
    unittest.main()
